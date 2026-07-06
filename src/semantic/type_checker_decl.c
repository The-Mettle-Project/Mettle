// Type checker: struct / enum / declaration processing.
#include "type_checker_internal.h"

// Struct type processing functions

int type_checker_process_struct_declaration(TypeChecker *checker,
                                            ASTNode *struct_decl) {
  if (!checker || !struct_decl || struct_decl->type != AST_STRUCT_DECLARATION) {
    return 0;
  }

  StructDeclaration *decl = (StructDeclaration *)struct_decl->data;
  if (!decl || !decl->name) {
    return 0;
  }

  // Check if struct already exists
  Symbol *existing =
      symbol_table_lookup_current_scope(checker->symbol_table, decl->name);
  if (existing) {
    type_checker_report_duplicate_declaration(checker, struct_decl->location,
                                              decl->name);
    return 0;
  }

  /* Self-referential structs (e.g. `next: Foo*` inside `struct Foo`) need
   * `Foo` resolvable as a base type while its own fields are being processed.
   * Register an empty placeholder struct type + symbol first; the pointer-type
   * parser only requires the base Type pointer to exist, not for its fields
   * to be populated. We fill in the field information in place once the
   * field types have all resolved. */
  Type *struct_type = type_create(TYPE_STRUCT, decl->name);
  if (!struct_type) {
    return 0;
  }

  Symbol *struct_symbol = symbol_create(decl->name, SYMBOL_STRUCT, struct_type);
  if (!struct_symbol) {
    type_destroy(struct_type);
    return 0;
  }

  if (!symbol_table_declare(checker->symbol_table, struct_symbol)) {
    symbol_destroy(struct_symbol);
    return 0;
  }

  // Resolve field types now that the placeholder is visible.
  Type **field_types = malloc(decl->field_count * sizeof(Type *));
  if (!field_types) {
    return 0;
  }

  for (size_t i = 0; i < decl->field_count; i++) {
    field_types[i] =
        type_checker_get_type_by_name(checker, decl->field_types[i]);
    if (!field_types[i]) {
      char error_msg[512];
      snprintf(error_msg, sizeof(error_msg),
               "Unknown type '%s' in struct field", decl->field_types[i]);
      type_checker_set_error_at_location(checker, struct_decl->location,
                                         error_msg);
      free(field_types);
      return 0;
    }
  }

  // Populate the placeholder in place: copy field names, attach field types,
  // and compute offsets/size/alignment. Mirrors type_create_struct, but
  // operates on the already-registered placeholder so that any pointers
  // captured during field resolution remain valid.
  struct_type->field_count = decl->field_count;
  struct_type->field_names = malloc(decl->field_count * sizeof(char *));
  struct_type->field_types = malloc(decl->field_count * sizeof(Type *));
  struct_type->field_offsets = malloc(decl->field_count * sizeof(size_t));
  if (!struct_type->field_names || !struct_type->field_types ||
      !struct_type->field_offsets) {
    free(field_types);
    return 0;
  }

  size_t current_offset = 0;
  size_t max_alignment = 1;
  for (size_t i = 0; i < decl->field_count; i++) {
    struct_type->field_names[i] = (char *)string_intern(decl->field_names[i]);
    struct_type->field_types[i] = field_types[i];
    size_t field_alignment = field_types[i]->alignment;
    if (field_alignment == 0) {
      /* A pointer to a not-yet-populated struct still has pointer size and
       * alignment because pointers are sized independently of their pointee.
       * Other zero-alignment cases shouldn't happen for first-class types. */
      field_alignment = 1;
    }
    if (field_alignment > max_alignment) {
      max_alignment = field_alignment;
    }
    size_t padding = (field_alignment - (current_offset % field_alignment)) %
                     field_alignment;
    current_offset += padding;
    struct_type->field_offsets[i] = current_offset;
    current_offset += field_types[i]->size;
  }
  size_t final_padding =
      (max_alignment - (current_offset % max_alignment)) % max_alignment;
  struct_type->size = current_offset + final_padding;
  struct_type->alignment = max_alignment;

  free(field_types);
  return 1;
}

// ---------------------------------------------------------------------------
// Helper: build a concrete TYPE_TAGGED_ENUM type and register its constructors.
// Called for plain (non-generic) tagged enum declarations and from the
// generic-instantiation path when we monomorphize "Option<int32>" on demand.
//
// The memory layout is:
//   offset 0              : int32 _tag  (4 bytes)
//   offset data_offset    : payload union (largest payload, alignment-padded)
// ---------------------------------------------------------------------------
Type *type_checker_build_tagged_enum_type(TypeChecker *checker,
                                                  const char *type_name,
                                                  EnumDeclaration *enum_decl) {
  if (!checker || !type_name || !enum_decl)
    return NULL;

  // Determine the max payload size and alignment
  size_t max_payload_size = 0;
  size_t max_payload_align = 1;
  for (size_t i = 0; i < enum_decl->variant_count; i++) {
    const char *pt = enum_decl->variants[i].payload_type;
    if (!pt)
      continue;
    Type *payload_ty = type_checker_get_type_by_name(checker, pt);
    if (!payload_ty)
      continue;
    if (payload_ty->size > max_payload_size)
      max_payload_size = payload_ty->size;
    if (payload_ty->alignment > max_payload_align)
      max_payload_align = payload_ty->alignment;
  }

  // data starts at first offset >= 4 that satisfies alignment of payload
  size_t data_align = max_payload_align < 4 ? 4 : max_payload_align;
  // align_up(4, data_align) – tag is 4 bytes, then pad to data_align
  size_t data_offset = (4 + data_align - 1) & ~(data_align - 1);
  size_t total_size = max_payload_size > 0 ? data_offset + max_payload_size
                                           : data_offset;
  // Round up total to alignment
  total_size = (total_size + data_align - 1) & ~(data_align - 1);
  if (total_size < 8) total_size = 8; // at least 8 bytes

  Type *te = type_create(TYPE_TAGGED_ENUM, type_name);
  if (!te)
    return NULL;

  te->size = total_size;
  te->alignment = data_align;
  te->tagged_variant_count = enum_decl->variant_count;
  te->tagged_data_offset = data_offset;
  te->tagged_data_size = max_payload_size;

  te->tagged_variant_names =
      malloc(enum_decl->variant_count * sizeof(char *));
  te->tagged_variant_tags =
      malloc(enum_decl->variant_count * sizeof(int));
  te->tagged_variant_payloads =
      malloc(enum_decl->variant_count * sizeof(Type *));

  if (!te->tagged_variant_names || !te->tagged_variant_tags ||
      !te->tagged_variant_payloads) {
    type_destroy(te);
    return NULL;
  }

  for (size_t i = 0; i < enum_decl->variant_count; i++) {
    te->tagged_variant_names[i] =
        strdup(enum_decl->variants[i].name);
    te->tagged_variant_tags[i] = (int)i;
    const char *pt = enum_decl->variants[i].payload_type;
    te->tagged_variant_payloads[i] =
        pt ? type_checker_get_type_by_name(checker, pt) : NULL;
  }

  return te;
}

int type_checker_process_tagged_enum(TypeChecker *checker,
                                            ASTNode *enum_decl_node) {
  EnumDeclaration *enum_decl = (EnumDeclaration *)enum_decl_node->data;

  Type *te =
      type_checker_build_tagged_enum_type(checker, enum_decl->name, enum_decl);
  if (!te) {
    type_checker_set_error_at_location(checker, enum_decl_node->location,
                                       "Failed to create tagged enum type '%s'",
                                       enum_decl->name);
    return 0;
  }

  Symbol *enum_sym =
      symbol_create(enum_decl->name, SYMBOL_ENUM, te);
  if (!enum_sym) {
    type_destroy(te);
    return 0;
  }
  if (!symbol_table_declare(checker->symbol_table, enum_sym)) {
    symbol_destroy(enum_sym);
    return 0;
  }

  // Register a constructor symbol for each variant
  for (size_t i = 0; i < enum_decl->variant_count; i++) {
    Symbol *ctor = symbol_create(enum_decl->variants[i].name,
                                 SYMBOL_TAGGED_ENUM_CONSTRUCTOR, te);
    if (!ctor)
      return 0;
    ctor->data.constructor.enum_type = te;
    ctor->data.constructor.tag_value = (int)i;
    ctor->data.constructor.payload_type = te->tagged_variant_payloads[i];
    ctor->is_initialized = 1;
    symbol_table_insert(checker->symbol_table, ctor);
  }

  return 1;
}

// ---------------------------------------------------------------------------
// Instantiate a generic enum template for a concrete type argument string.
// e.g. "Option<int32>" → creates and registers Option__int32 if needed.
// ---------------------------------------------------------------------------
Type *type_checker_instantiate_generic_enum(TypeChecker *checker,
                                                    const char *generic_name,
                                                    const char *type_arg_str) {
  if (!checker || !generic_name || !type_arg_str)
    return NULL;

  // Build mangled name  e.g. Option__int32
  size_t mangled_len =
      strlen(generic_name) + 2 + strlen(type_arg_str) + 1;
  char *mangled = malloc(mangled_len);
  if (!mangled)
    return NULL;
  snprintf(mangled, mangled_len, "%s__%s", generic_name, type_arg_str);

  // Return cached type if already instantiated
  Type *existing = type_checker_get_type_by_name(checker, mangled);
  if (existing) {
    free(mangled);
    return existing;
  }

  // Resolve the type argument
  Type *arg_type = type_checker_get_type_by_name(checker, type_arg_str);
  if (!arg_type) {
    free(mangled);
    return NULL;
  }

  // Find the matching generic enum template
  ASTNode *template_node = NULL;
  for (size_t i = 0; i < checker->generic_enum_template_count; i++) {
    ASTNode *n = checker->generic_enum_templates[i];
    if (!n) continue;
    EnumDeclaration *ed = (EnumDeclaration *)n->data;
    if (ed && ed->name && strcmp(ed->name, generic_name) == 0) {
      template_node = n;
      break;
    }
  }
  if (!template_node) {
    free(mangled);
    return NULL;
  }

  EnumDeclaration *tmpl = (EnumDeclaration *)template_node->data;

  // Build a transient EnumDeclaration with T substituted by the concrete type
  EnumVariant *concrete_variants =
      malloc(tmpl->variant_count * sizeof(EnumVariant));
  if (!concrete_variants) {
    free(mangled);
    return NULL;
  }
  for (size_t i = 0; i < tmpl->variant_count; i++) {
    concrete_variants[i].name = tmpl->variants[i].name;
    concrete_variants[i].value = NULL;
    // Substitute type parameter: if payload_type == type_param[0] → use arg
    const char *orig_pt = tmpl->variants[i].payload_type;
    if (orig_pt && tmpl->type_param_count > 0 &&
        strcmp(orig_pt, tmpl->type_params[0]) == 0) {
      concrete_variants[i].payload_type = (char *)type_arg_str;
    } else {
      concrete_variants[i].payload_type = (char *)orig_pt;
    }
  }

  EnumDeclaration concrete_decl;
  concrete_decl.name = mangled;
  concrete_decl.variants = concrete_variants;
  concrete_decl.variant_count = tmpl->variant_count;
  concrete_decl.is_exported = 0;
  concrete_decl.type_params = NULL;
  concrete_decl.type_param_count = 0;

  Type *te =
      type_checker_build_tagged_enum_type(checker, mangled, &concrete_decl);
  free(concrete_variants);

  if (!te) {
    free(mangled);
    return NULL;
  }

  // Register type + constructors in symbol table
  Symbol *enum_sym = symbol_create(mangled, SYMBOL_ENUM, te);
  free(mangled);
  if (!enum_sym) {
    type_destroy(te);
    return NULL;
  }
  symbol_table_insert(checker->symbol_table, enum_sym);

  // Constructors use variant-qualified names: mangled__VariantName
  for (size_t i = 0; i < tmpl->variant_count; i++) {
    // Also register bare variant names if not already in scope
    // (bare names are variant constructors for the first instantiation seen)
    Symbol *existing_ctor = symbol_table_lookup(
        checker->symbol_table, tmpl->variants[i].name);
    if (!existing_ctor) {
      Symbol *ctor = symbol_create(tmpl->variants[i].name,
                                   SYMBOL_TAGGED_ENUM_CONSTRUCTOR, te);
      if (ctor) {
        ctor->data.constructor.enum_type = te;
        ctor->data.constructor.tag_value = (int)i;
        ctor->data.constructor.payload_type = te->tagged_variant_payloads[i];
        ctor->is_initialized = 1;
        symbol_table_insert(checker->symbol_table, ctor);
      }
    }
  }

  return te;
}

int type_checker_process_enum_declaration(TypeChecker *checker,
                                          ASTNode *enum_decl_node) {
  if (!checker || !enum_decl_node ||
      enum_decl_node->type != AST_ENUM_DECLARATION) {
    return 0;
  }

  EnumDeclaration *enum_decl = (EnumDeclaration *)enum_decl_node->data;
  if (!enum_decl || !enum_decl->name) {
    type_checker_set_error_at_location(checker, enum_decl_node->location,
                                       "Invalid enum declaration");
    return 0;
  }

  // If this enum has type parameters it's a generic template — store the AST
  // node for later monomorphization and do not register a concrete type now.
  if (enum_decl->type_param_count > 0) {
    ASTNode **new_tmpl = realloc(
        checker->generic_enum_templates,
        (checker->generic_enum_template_count + 1) * sizeof(ASTNode *));
    if (!new_tmpl)
      return 0;
    checker->generic_enum_templates = new_tmpl;
    checker->generic_enum_templates[checker->generic_enum_template_count++] =
        enum_decl_node;
    return 1;
  }

  // Check whether any variant carries a payload — if so, it's a tagged enum.
  int is_tagged = 0;
  for (size_t i = 0; i < enum_decl->variant_count; i++) {
    if (enum_decl->variants[i].payload_type) {
      is_tagged = 1;
      break;
    }
  }

  // Check for duplicate type declaration
  if (type_checker_get_type_by_name(checker, enum_decl->name)) {
    type_checker_set_error_at_location(checker, enum_decl_node->location,
                                       "Type '%s' already declared",
                                       enum_decl->name);
    return 0;
  }

  if (is_tagged) {
    return type_checker_process_tagged_enum(checker, enum_decl_node);
  }

  // Plain (integer-valued) enum.
  Type *new_enum_type = type_create(TYPE_ENUM, enum_decl->name);
  if (!new_enum_type) {
    type_checker_set_error_at_location(checker, enum_decl_node->location,
                                       "Failed to create enum type");
    return 0;
  }
  new_enum_type->size = 8;
  new_enum_type->alignment = 8;

  Symbol *enum_symbol =
      symbol_create(enum_decl->name, SYMBOL_ENUM, new_enum_type);
  if (!enum_symbol) {
    type_destroy(new_enum_type);
    return 0;
  }
  if (!symbol_table_declare(checker->symbol_table, enum_symbol)) {
    symbol_destroy(enum_symbol);
    return 0;
  }

  long long current_val = 0;
  for (size_t i = 0; i < enum_decl->variant_count; i++) {
    EnumVariant *variant = &enum_decl->variants[i];

    if (variant->value) {
      ASTNode *val_node = variant->value;
      if (val_node->type == AST_NUMBER_LITERAL) {
        current_val = ((NumberLiteral *)val_node->data)->int_value;
      } else if (val_node->type == AST_UNARY_EXPRESSION &&
                 ((UnaryExpression *)val_node->data)->operand->type ==
                     AST_NUMBER_LITERAL &&
                 strcmp(((UnaryExpression *)val_node->data)->operator, "-") ==
                     0) {
        current_val = -((NumberLiteral *)((UnaryExpression *)val_node->data)
                            ->operand->data)
                           ->int_value;
      } else {
        type_checker_set_error_at_location(
            checker, val_node->location,
            "Enum variant initializer must be a constant integer");
        return 0;
      }
    }

    if (symbol_table_lookup_current_scope(checker->symbol_table,
                                          variant->name)) {
      type_checker_report_duplicate_declaration(
          checker, enum_decl_node->location, variant->name);
      return 0;
    }

    Symbol *sym = symbol_create(variant->name, SYMBOL_CONSTANT, new_enum_type);
    if (!sym)
      return 0;
    sym->data.constant.value = current_val;
    sym->is_initialized = 1;
    symbol_table_insert(checker->symbol_table, sym);
    current_val++;
  }

  return 1;
}

int type_checker_process_declaration(TypeChecker *checker,
                                     ASTNode *declaration) {
  if (!checker || !declaration) {
    return 0;
  }

  switch (declaration->type) {
  case AST_DEFER_STATEMENT:
    type_checker_set_error_at_location(checker, declaration->location,
                                       "Defer statement outside of a function");
    return 0;

  case AST_ERRDEFER_STATEMENT:
    type_checker_set_error_at_location(
        checker, declaration->location,
        "Errdefer statement outside of a function");
    return 0;

  case AST_FUNCTION_CALL: {
    CallExpression *call = (CallExpression *)declaration->data;
    if (call && call->function_name &&
        strcmp(call->function_name, "static_assert") == 0) {
      return type_checker_validate_static_assert(checker, call,
                                                 declaration->location);
    }
    type_checker_set_error_at_location(
        checker, declaration->location,
        "Unsupported top-level construct in declaration context");
    return 0;
  }

  case AST_VAR_DECLARATION: {
    VarDeclaration *var_decl = (VarDeclaration *)declaration->data;
    if (!var_decl || !var_decl->name) {
      type_checker_set_error_at_location(checker, declaration->location,
                                         "Invalid variable declaration");
      return 0;
    }

    if (var_decl->link_name && !var_decl->is_extern) {
      type_checker_set_error_at_location(
          checker, declaration->location,
          "Link-name suffix is only allowed on extern declarations");
      return 0;
    }

    Scope *current_scope =
        symbol_table_get_current_scope(checker->symbol_table);
    if (var_decl->is_extern &&
        (!current_scope || current_scope->type != SCOPE_GLOBAL)) {
      type_checker_set_error_at_location(
          checker, declaration->location,
          "Extern declarations are only allowed at top level");
      return 0;
    }

    if (var_decl->is_extern && var_decl->initializer) {
      type_checker_set_error_at_location(
          checker, declaration->location,
          "Extern variable '%s' cannot have an initializer", var_decl->name);
      return 0;
    }
    if (var_decl->is_extern && !var_decl->type_name) {
      type_checker_set_error_at_location(
          checker, declaration->location,
          "Extern variable '%s' requires an explicit type annotation",
          var_decl->name);
      return 0;
    }

    Type *var_type = NULL;

    // If type is explicitly specified, resolve it
    if (var_decl->type_name) {
      var_type = type_checker_get_type_by_name(checker, var_decl->type_name);
      if (!var_type) {
        type_checker_report_undefined_symbol(checker, declaration->location,
                                             var_decl->type_name, "type");
        return 0;
      }
    }

    // If there's an initializer, validate it. When validation fails but the
    // declared type is known, the variable is still registered with that type
    // ("poisoned") so later uses don't cascade into bogus undefined-variable
    // errors; the declaration itself still fails.
    int poisoned = 0;
    if (var_decl->initializer) {
      size_t reports_before =
          checker->error_reporter ? checker->error_reporter->count : 0;
      Type *init_type = type_checker_infer_type(checker, var_decl->initializer);
      if (!init_type) {
        int already_reported =
            checker->error_reporter
                ? checker->error_reporter->count > reports_before
                : checker->has_error;
        if (!already_reported) {
          type_checker_set_error_at_location(
              checker, var_decl->initializer->location,
              "Cannot infer type of initializer for variable '%s'",
              var_decl->name);
        }
        checker->has_error = 1;
        if (!var_type)
          return 0;
        poisoned = 1;
      }
      if (!poisoned && var_type) {
        /* A capturing closure carries a heap environment and cannot be stored in
         * a plain function-pointer type; it needs a closure type `Fn(...)`. */
        if (init_type && init_type->kind == TYPE_FUNCTION_POINTER &&
            init_type->closure_env &&
            !(var_type->kind == TYPE_FUNCTION_POINTER && var_type->closure_env)) {
          type_checker_set_error_at_location(
              checker, var_decl->initializer->location,
              "a capturing closure cannot be stored in a plain function-pointer "
              "type '%s'; declare '%s' with a closure type 'Fn(...)' instead",
              var_type->name, var_decl->name);
          poisoned = 1;
        }
        // Type specified: validate assignment compatibility
        else if (!(var_type->kind == TYPE_POINTER &&
              type_checker_is_null_pointer_constant(var_decl->initializer)) &&
            !type_checker_is_assignable(checker, var_type, init_type)) {
          type_checker_report_type_mismatch_node(checker, var_decl->initializer,
                                                 var_type->name,
                                                 init_type->name);
          poisoned = 1;
        }
      } else if (poisoned) {
        /* Initializer failed but declared type is known: register anyway. */
      } else if (var_decl->structural_type ||
                 (var_decl->is_const &&
                  (!current_scope || current_scope->type == SCOPE_GLOBAL))) {
        // Exempt: a compiler-synthesized binding whose type is structural (e.g.
        // a range-`for` counter), or a global `const` (integer-only and folded
        // at each use, so its type is exactly its literal value's type). Take
        // the initializer type.
        var_type = init_type;
      } else {
        // Mettle requires an explicit type on every user `var` and local
        // `const` binding; nothing is inferred from an arbitrary initializer.
        type_checker_set_error_at_location(
            checker, declaration->location,
            "%s '%s' requires an explicit type: write '%s %s: <type> = ...' "
            "(Mettle does not infer binding types)",
            var_decl->is_const ? "constant" : "variable", var_decl->name,
            var_decl->is_const ? "const" : "var", var_decl->name);
        return 0;
      }
    } else if (!var_type) {
      type_checker_set_error_at_location(
          checker, declaration->location,
          "Variable '%s' must have either a type annotation or an initializer",
          var_decl->name);
      return 0;
    }

    // A `const` declaration binds an immutable value and must be initialized.
    // An integer const is folded at every use site (SYMBOL_CONSTANT) at global
    // scope and needs no storage. A const of any other type (float, string,
    // ...) cannot be folded, so it is registered as an immutable variable with
    // normal storage and initializer codegen. Reassignment is rejected via the
    // immutable flag in either case.
    if (var_decl->is_const) {
      if (!var_decl->initializer) {
        type_checker_set_error_at_location(
            checker, declaration->location,
            "Constant '%s' must have an initializer", var_decl->name);
        return 0;
      }
      // Integer consts fold to a compile-time value: at global scope they are
      // registered as SYMBOL_CONSTANT (folded at every use, no storage). A
      // non-integer const (float/string/aggregate) is not folded; it falls
      // through to immutable-variable registration below and gets normal global
      // (or local) storage. The initializer's assignability was validated above.
      if (!poisoned && type_checker_is_integer_type(var_type)) {
        long long const_value = 0;
        if (!type_checker_eval_integer_constant_with_checker(
                checker, var_decl->initializer, &const_value)) {
          type_checker_set_error_at_location(
              checker, var_decl->initializer->location,
              "Constant '%s' initializer must be a compile-time integer "
              "constant expression",
              var_decl->name);
          return 0;
        }
        if (current_scope && current_scope->type == SCOPE_GLOBAL) {
          if (symbol_table_lookup_current_scope(checker->symbol_table,
                                                var_decl->name)) {
            type_checker_report_duplicate_declaration(
                checker, declaration->location, var_decl->name);
            return 0;
          }
          Symbol *const_symbol =
              symbol_create(var_decl->name, SYMBOL_CONSTANT, var_type);
          if (!const_symbol) {
            type_checker_set_error_at_location(
                checker, declaration->location,
                "Failed to create symbol for constant '%s'", var_decl->name);
            return 0;
          }
          const_symbol->data.constant.value = const_value;
          const_symbol->is_initialized = 1;
          if (!symbol_table_declare(checker->symbol_table, const_symbol)) {
            type_checker_report_duplicate_declaration(
                checker, declaration->location, var_decl->name);
            symbol_destroy(const_symbol);
            return 0;
          }
          return 1;
        }
        // Local integer const: fall through to immutable variable registration.
      }
      // Local const (any type) and non-integer global const (float, string,
      // ...): fall through to immutable-variable registration; storage and the
      // initializer are emitted like a normal variable, and the immutable flag
      // below rejects reassignment. Global float/string globals now load
      // correctly in the direct-object backend, so they are no longer rejected.
    }

    // Check for duplicate declaration in current scope.
    Symbol *existing = symbol_table_lookup_current_scope(checker->symbol_table,
                                                         var_decl->name);
    if (existing) {
      if (existing->kind != SYMBOL_VARIABLE) {
        type_checker_report_duplicate_declaration_prev(
            checker, declaration->location, var_decl->name, existing);
        return 0;
      }
      if (existing->is_extern != var_decl->is_extern) {
        type_checker_set_error_at_location(
            checker, declaration->location,
            "Variable '%s' redeclared with conflicting extern/non-extern "
            "linkage",
            var_decl->name);
        return 0;
      }
      if (!var_decl->is_extern) {
        type_checker_report_duplicate_declaration_prev(
            checker, declaration->location, var_decl->name, existing);
        return 0;
      }
      if (!type_checker_types_equal(existing->type, var_type)) {
        type_checker_set_error_at_location(
            checker, declaration->location,
            "Extern variable '%s' redeclared with conflicting type",
            var_decl->name);
        return 0;
      }
      if (!type_checker_link_name_matches_symbol(existing, var_decl->name,
                                                 var_decl->is_extern,
                                                 var_decl->link_name)) {
        type_checker_set_error_at_location(
            checker, declaration->location,
            "Extern variable '%s' redeclared with conflicting link name",
            var_decl->name);
        return 0;
      }
      return 1;
    }

    // Create and declare the symbol
    Symbol *var_symbol =
        symbol_create(var_decl->name, SYMBOL_VARIABLE, var_type);
    if (var_symbol) {
      var_symbol->decl_line = declaration->location.line;
      var_symbol->decl_column = declaration->location.column;
      var_symbol->decl_file = declaration->location.filename;
    }
    if (!var_symbol) {
      type_checker_set_error_at_location(
          checker, declaration->location,
          "Failed to create symbol for variable '%s'", var_decl->name);
      return 0;
    }

    var_symbol->is_extern = var_decl->is_extern;
    var_symbol->is_immutable = var_decl->is_const;
    if (var_decl->is_extern) {
      const char *effective_link_name = type_checker_decl_link_name(
          var_decl->name, var_decl->is_extern, var_decl->link_name);
      var_symbol->link_name =
          effective_link_name ? strdup(effective_link_name) : NULL;
      if (!var_symbol->link_name) {
        type_checker_set_error_at_location(
            checker, declaration->location,
            "Failed to allocate link name for extern variable '%s'",
            var_decl->name);
        symbol_destroy(var_symbol);
        return 0;
      }
    }

    if (!symbol_table_declare(checker->symbol_table, var_symbol)) {
      type_checker_report_duplicate_declaration_prev(
          checker, declaration->location, var_decl->name,
          symbol_table_lookup_current_scope(checker->symbol_table,
                                            var_decl->name));
      symbol_destroy(var_symbol);
      return 0;
    }

    if (checker->current_function && !var_decl->is_extern) {
      Scope *declare_scope =
          symbol_table_get_current_scope(checker->symbol_table);
      if (declare_scope && declare_scope->type != SCOPE_GLOBAL) {
        int track_definite_init =
            !(var_type &&
              (var_type->kind == TYPE_ARRAY || var_type->kind == TYPE_STRUCT ||
               var_type->kind == TYPE_STRING));
        if (track_definite_init) {
          if (!type_checker_init_tracker_declare(
                  checker, var_decl->name, var_decl->initializer != NULL)) {
            type_checker_set_error_at_location(
                checker, declaration->location,
                "Out of memory while tracking initialization state for '%s'",
                var_decl->name);
            return 0;
          }
        }

        if (var_type && var_type->kind == TYPE_POINTER) {
          long long known_extent = type_checker_extract_known_buffer_extent(
              checker, var_decl->initializer);
          long long known_alignment =
              type_checker_extract_known_pointer_alignment(
                  checker, var_decl->initializer);
          if (!type_checker_buffer_extent_declare(checker, var_decl->name,
                                                  known_extent,
                                                  known_alignment)) {
            type_checker_set_error_at_location(
                checker, declaration->location,
                "Out of memory while tracking buffer extent for '%s'",
                var_decl->name);
            return 0;
          }
        }
      }
    }

    return poisoned ? 0 : 1;
  }

  case AST_FUNCTION_DECLARATION: {
    FunctionDeclaration *func_decl = (FunctionDeclaration *)declaration->data;
    if (!func_decl || !func_decl->name) {
      type_checker_set_error_at_location(checker, declaration->location,
                                         "Invalid function declaration");
      return 0;
    }

    if (func_decl->link_name && !func_decl->is_extern) {
      type_checker_set_error_at_location(
          checker, declaration->location,
          "Link-name suffix is only allowed on extern declarations");
      return 0;
    }

    Scope *current_scope =
        symbol_table_get_current_scope(checker->symbol_table);
    if (func_decl->is_extern &&
        (!current_scope || current_scope->type != SCOPE_GLOBAL)) {
      type_checker_set_error_at_location(
          checker, declaration->location,
          "Extern declarations are only allowed at top level");
      return 0;
    }
    if (func_decl->is_extern && func_decl->body) {
      type_checker_set_error_at_location(
          checker, declaration->location,
          "Extern function '%s' must not have a body", func_decl->name);
      return 0;
    }

    // Resolve return type
    Type *return_type = NULL;
    if (func_decl->return_type) {
      return_type =
          type_checker_get_type_by_name(checker, func_decl->return_type);
      if (!return_type) {
        type_checker_report_undefined_symbol(checker, declaration->location,
                                             func_decl->return_type, "type");
        return 0;
      }
    } else {
      return_type = checker->builtin_void;
    }

    // Resolve parameter types and check for duplicate parameter names
    Type **param_types = NULL;
    if (func_decl->parameter_count > 0) {
      param_types = malloc(func_decl->parameter_count * sizeof(Type *));
      if (!param_types) {
        type_checker_set_error_at_location(
            checker, declaration->location,
            "Memory allocation failed for function parameters");
        return 0;
      }

      // Check for duplicate parameter names
      for (size_t i = 0; i < func_decl->parameter_count; i++) {
        for (size_t j = i + 1; j < func_decl->parameter_count; j++) {
          if (strcmp(func_decl->parameter_names[i],
                     func_decl->parameter_names[j]) == 0) {
            type_checker_report_duplicate_declaration(
                checker, declaration->location, func_decl->parameter_names[i]);
            free(param_types);
            return 0;
          }
        }
      }

      for (size_t i = 0; i < func_decl->parameter_count; i++) {
        param_types[i] = type_checker_get_type_by_name(
            checker, func_decl->parameter_types[i]);
        if (!param_types[i]) {
          type_checker_report_undefined_symbol(checker, declaration->location,
                                               func_decl->parameter_types[i],
                                               "type");
          free(param_types);
          return 0;
        }
      }
    }

    // Copy parameter names so function symbols own their metadata.
    char **param_names_copy = NULL;
    if (func_decl->parameter_count > 0) {
      param_names_copy = malloc(func_decl->parameter_count * sizeof(char *));
      if (!param_names_copy) {
        if (param_types)
          free(param_types);
        type_checker_set_error_at_location(
            checker, declaration->location,
            "Memory allocation failed for function parameter names");
        return 0;
      }
      for (size_t i = 0; i < func_decl->parameter_count; i++) {
        param_names_copy[i] = strdup(func_decl->parameter_names[i]);
        if (!param_names_copy[i]) {
          for (size_t j = 0; j < i; j++) {
            free(param_names_copy[j]);
          }
          free(param_names_copy);
          if (param_types)
            free(param_types);
          type_checker_set_error_at_location(
              checker, declaration->location,
              "Memory allocation failed for parameter name copy");
          return 0;
        }
      }
    }

    // Create function symbol
    Symbol *func_symbol =
        symbol_create(func_decl->name, SYMBOL_FUNCTION, return_type);
    if (!func_symbol) {
      type_checker_set_error_at_location(
          checker, declaration->location,
          "Memory allocation failed for function symbol");
      if (param_names_copy) {
        for (size_t i = 0; i < func_decl->parameter_count; i++) {
          free(param_names_copy[i]);
        }
        free(param_names_copy);
      }
      if (param_types)
        free(param_types);
      return 0;
    }

    // Set function-specific data
    func_symbol->data.function.parameter_count = func_decl->parameter_count;
    func_symbol->data.function.parameter_names = param_names_copy;
    func_symbol->data.function.parameter_types = param_types;
    func_symbol->data.function.return_type = return_type;
    func_symbol->is_extern = func_decl->is_extern;
    if (func_decl->is_extern) {
      const char *effective_link_name = type_checker_decl_link_name(
          func_decl->name, func_decl->is_extern, func_decl->link_name);
      func_symbol->link_name =
          effective_link_name ? strdup(effective_link_name) : NULL;
      if (!func_symbol->link_name) {
        type_checker_set_error_at_location(
            checker, declaration->location,
            "Failed to allocate link name for extern function '%s'",
            func_decl->name);
        symbol_destroy(func_symbol);
        return 0;
      }
    }

    Symbol *existing_before = symbol_table_lookup_current_scope(
        checker->symbol_table, func_decl->name);
    int is_resolving_forward =
        (existing_before && existing_before->kind == SYMBOL_FUNCTION &&
         existing_before->is_forward_declaration);

    if (existing_before && existing_before->kind != SYMBOL_FUNCTION) {
      type_checker_report_duplicate_declaration(checker, declaration->location,
                                                func_decl->name);
      symbol_destroy(func_symbol);
      return 0;
    }

    if (existing_before && existing_before->kind == SYMBOL_FUNCTION) {
      if (existing_before->is_extern != func_decl->is_extern) {
        type_checker_set_error_at_location(
            checker, declaration->location,
            "Function '%s' redeclared with conflicting extern/non-extern "
            "linkage",
            func_decl->name);
        symbol_destroy(func_symbol);
        return 0;
      }
      if ((existing_before->is_extern || func_decl->is_extern) &&
          !type_checker_link_name_matches_symbol(
              existing_before, func_decl->name, func_decl->is_extern,
              func_decl->link_name)) {
        type_checker_set_error_at_location(
            checker, declaration->location,
            "Function '%s' redeclared with conflicting link name",
            func_decl->name);
        symbol_destroy(func_symbol);
        return 0;
      }
    }

    // Forward declaration: no body
    if (!func_decl->body) {
      func_symbol->is_initialized = 0;
      if (!symbol_table_declare_forward(checker->symbol_table, func_symbol)) {
        type_checker_set_error_at_location(
            checker, declaration->location,
            "Invalid or conflicting forward declaration for function '%s'",
            func_decl->name);
        symbol_destroy(func_symbol);
        return 0;
      }
      if (is_resolving_forward) {
        symbol_destroy(func_symbol);
      }
      return 1;
    }

    func_symbol->is_initialized = 1;
    if (!symbol_table_resolve_forward_declaration(checker->symbol_table,
                                                  func_symbol)) {
      type_checker_set_error_at_location(
          checker, declaration->location,
          "Function definition for '%s' does not match existing declaration",
          func_decl->name);
      symbol_destroy(func_symbol);
      return 0;
    }

    if (is_resolving_forward) {
      checker->current_function = existing_before;
      symbol_destroy(func_symbol); // not inserted, existing symbol was updated
      func_symbol = existing_before;
    } else {
      checker->current_function = func_symbol;
    }
    checker->current_function_decl = declaration;

    // Enter a new scope for the function body
    if (!symbol_table_enter_scope(checker->symbol_table, SCOPE_FUNCTION)) {
      type_checker_set_error_at_location(
          checker, declaration->location,
          "Out of memory while entering function scope");
      return 0;
    }
    type_checker_init_tracker_reset(checker);
    if (!type_checker_init_tracker_enter_scope(checker)) {
      type_checker_set_error_at_location(
          checker, declaration->location,
          "Out of memory while initializing flow analysis scope");
      symbol_table_exit_scope(checker->symbol_table);
      return 0;
    }

    // Add parameters to the new scope
    Type **active_param_types =
        checker->current_function->data.function.parameter_types;
    if (func_decl->parameter_count > 0) {
      for (size_t i = 0; i < func_decl->parameter_count; i++) {
        Symbol *param_symbol =
            symbol_create(func_decl->parameter_names[i], SYMBOL_VARIABLE,
                          active_param_types ? active_param_types[i] : NULL);
        if (!param_symbol) {
          type_checker_set_error_at_location(
              checker, declaration->location,
              "Failed to create parameter symbol");
          symbol_table_exit_scope(checker->symbol_table);
          return 0;
        }
        if (!symbol_table_declare(checker->symbol_table, param_symbol)) {
          type_checker_report_duplicate_declaration(
              checker, declaration->location, func_decl->parameter_names[i]);
          symbol_destroy(param_symbol);
          type_checker_init_tracker_reset(checker);
          symbol_table_exit_scope(checker->symbol_table);
          return 0;
        }
        if (!type_checker_init_tracker_declare(
                checker, func_decl->parameter_names[i], 1)) {
          type_checker_set_error_at_location(
              checker, declaration->location,
              "Out of memory while tracking parameter initialization");
          type_checker_init_tracker_reset(checker);
          symbol_table_exit_scope(checker->symbol_table);
          return 0;
        }
        Type *param_type = active_param_types ? active_param_types[i] : NULL;
        if (param_type && param_type->kind == TYPE_POINTER) {
          if (!type_checker_buffer_extent_declare(
                  checker, func_decl->parameter_names[i], -1, -1)) {
            type_checker_set_error_at_location(
                checker, declaration->location,
                "Out of memory while tracking pointer parameter extent");
            type_checker_init_tracker_reset(checker);
            symbol_table_exit_scope(checker->symbol_table);
            return 0;
          }
        }
      }
    }

    // Process the function body
    if (func_decl->body &&
        !type_checker_check_statement(checker, func_decl->body)) {
      // Error already reported
      type_checker_init_tracker_reset(checker);
      symbol_table_exit_scope(checker->symbol_table);
      return 0;
    }

    // Memory diagnostics (use-after-free, dangling stack addresses,
    // constant out-of-bounds accesses, leaks). The scope is still live, so
    // `const` locals resolve for constant-index evaluation.
    if (func_decl->body &&
        !type_checker_check_function_memory(checker, declaration)) {
      type_checker_init_tracker_reset(checker);
      symbol_table_exit_scope(checker->symbol_table);
      return 0;
    }

    // A function with a non-void return type must contain at least one
    // return statement. This is a simple body-walk (a missing return on
    // some paths is not yet diagnosed); a function with no return at all
    // would otherwise compile and return garbage from RAX/XMM0. `main` is
    // exempt: the entry point falls through to an implicit `return 0`.
    if (func_decl->body && return_type &&
        return_type->kind != TYPE_VOID &&
        strcmp(func_decl->name, "main") != 0 &&
        !type_checker_ast_contains_node_type(func_decl->body,
                                             AST_RETURN_STATEMENT)) {
      type_checker_set_error_at_location(
          checker, declaration->location,
          "Function '%s' has non-void return type '%s' but contains no return "
          "statement",
          func_decl->name, return_type->name);
      type_checker_init_tracker_reset(checker);
      symbol_table_exit_scope(checker->symbol_table);
      return 0;
    }

    type_checker_init_tracker_exit_scope(checker);
    type_checker_init_tracker_reset(checker);

    // Exit the function's scope
    symbol_table_exit_scope(checker->symbol_table);

    // Reset the current function in the type checker
    checker->current_function = NULL;
    checker->current_function_decl = NULL;

    return 1;
  }

  case AST_METHOD_DECLARATION:
    // Method declarations are handled within struct processing
    // This case shouldn't normally be reached during standalone processing
    return 1;

  case AST_INLINE_ASM:
    // Top-level inline assembly is permitted.
    return 1;

  case AST_ASSIGNMENT: {
    Assignment *assignment = (Assignment *)declaration->data;
    if (!assignment || !assignment->value) {
      type_checker_set_error_at_location(checker, declaration->location,
                                         "Invalid assignment statement");
      return 0;
    }

    // Complex assignment target: obj.field = value or arr[i] = value
    if (assignment->target) {
      if (assignment->target->type == AST_MEMBER_ACCESS) {
        MemberAccess *member = (MemberAccess *)assignment->target->data;
        if (!member || !member->object || !member->member) {
          type_checker_set_error_at_location(checker,
                                             assignment->target->location,
                                             "Invalid field assignment target");
          return 0;
        }

        Type *object_type = type_checker_infer_type(checker, member->object);
        if (!object_type) {
          return 0;
        }
        /* Assigning through a pointer-to-struct auto-dereferences (like `->`). */
        if (object_type->kind == TYPE_POINTER && object_type->base_type) {
          object_type = object_type->base_type;
        }

        if (object_type->kind != TYPE_STRUCT &&
            object_type->kind != TYPE_STRING) {
          char error_msg[512];
          snprintf(error_msg, sizeof(error_msg),
                   "Cannot assign field '%s' on non-struct/string type '%s'",
                   member->member, object_type->name);
          type_checker_set_error_at_location(
              checker, assignment->target->location, error_msg);
          return 0;
        }

        Type *field_type = type_get_field_type(object_type, member->member);
        if (!field_type) {
          char error_msg[512];
          snprintf(error_msg, sizeof(error_msg),
                   "Field '%s' not found in type '%s'", member->member,
                   object_type->name);
          type_checker_set_error_at_location(
              checker, assignment->target->location, error_msg);
          return 0;
        }

        Type *value_type = type_checker_infer_type(checker, assignment->value);
        if (!value_type) {
          if (!checker->has_error) {
            type_checker_set_error_at_location(
                checker, assignment->value->location,
                "Cannot infer type of assignment value");
          }
          return 0;
        }

        if (!(field_type->kind == TYPE_POINTER &&
              type_checker_is_null_pointer_constant(assignment->value)) &&
            !type_checker_is_assignable(checker, field_type, value_type)) {
          type_checker_report_type_mismatch(checker,
                                            assignment->value->location,
                                            field_type->name, value_type->name);
          return 0;
        }

        return 1;
      } else if (assignment->target->type == AST_INDEX_EXPRESSION) {
        ArrayIndexExpression *target_index =
            (ArrayIndexExpression *)assignment->target->data;
        if (!target_index || !target_index->array || !target_index->index) {
          type_checker_set_error_at_location(checker,
                                             assignment->target->location,
                                             "Invalid array assignment target");
          return 0;
        }

        Type *target_array_type =
            type_checker_infer_type(checker, target_index->array);
        if (!target_array_type) {
          return 0;
        }
        if (target_array_type->kind == TYPE_ARRAY) {
          long long constant_index = 0;
          if (type_checker_eval_integer_constant(target_index->index,
                                                 &constant_index)) {
            if (constant_index < 0 ||
                (unsigned long long)constant_index >=
                    (unsigned long long)target_array_type->array_size) {
              type_checker_set_error_at_location(
                  checker, target_index->index->location,
                  "Array index %lld is out of bounds for '%s' (size %zu)",
                  constant_index,
                  target_array_type->name ? target_array_type->name : "array",
                  target_array_type->array_size);
              return 0;
            }
          }
        }

        Type *element_type =
            type_checker_infer_type(checker, assignment->target);
        if (!element_type) {
          return 0;
        }

        Type *value_type = type_checker_infer_type(checker, assignment->value);
        if (!value_type) {
          if (!checker->has_error) {
            type_checker_set_error_at_location(
                checker, assignment->value->location,
                "Cannot infer type of assignment value");
          }
          return 0;
        }

        if (!(element_type->kind == TYPE_POINTER &&
              type_checker_is_null_pointer_constant(assignment->value)) &&
            !type_checker_is_assignable(checker, element_type, value_type)) {
          type_checker_report_type_mismatch(
              checker, assignment->value->location, element_type->name,
              value_type->name);
          return 0;
        }

        return 1;
      } else if (assignment->target->type == AST_UNARY_EXPRESSION) {
        UnaryExpression *target_unary =
            (UnaryExpression *)assignment->target->data;
        if (!target_unary || !target_unary->operator ||
            strcmp(target_unary->operator, "*") != 0) {
          type_checker_set_error_at_location(checker,
                                             assignment->target->location,
                                             "Invalid assignment target");
          return 0;
        }

        Type *target_type =
            type_checker_infer_type(checker, assignment->target);
        if (!target_type) {
          return 0;
        }

        Type *value_type = type_checker_infer_type(checker, assignment->value);
        if (!value_type) {
          if (!checker->has_error) {
            type_checker_set_error_at_location(
                checker, assignment->value->location,
                "Cannot infer type of assignment value");
          }
          return 0;
        }

        if (!(target_type->kind == TYPE_POINTER &&
              type_checker_is_null_pointer_constant(assignment->value)) &&
            !type_checker_is_assignable(checker, target_type, value_type)) {
          type_checker_report_type_mismatch(
              checker, assignment->value->location, target_type->name,
              value_type->name);
          return 0;
        }

        return 1;
      }

      type_checker_set_error_at_location(checker, assignment->target->location,
                                         "Invalid assignment target");
      return 0;
    }

    // Simple variable assignment: name = value
    if (!assignment->variable_name) {
      type_checker_set_error_at_location(checker, declaration->location,
                                         "Invalid assignment statement");
      return 0;
    }

    // Look up the variable
    Symbol *var_symbol =
        symbol_table_lookup(checker->symbol_table, assignment->variable_name);
    if (!var_symbol) {
      type_checker_report_undefined_symbol(checker, declaration->location,
                                           assignment->variable_name,
                                           "variable");
      return 0;
    }

    if (var_symbol->kind != SYMBOL_VARIABLE &&
        var_symbol->kind != SYMBOL_PARAMETER) {
      char error_msg[512];
      const char *symbol_type =
          (var_symbol->kind == SYMBOL_FUNCTION)   ? "function"
          : (var_symbol->kind == SYMBOL_STRUCT)   ? "struct"
          : (var_symbol->kind == SYMBOL_CONSTANT) ? "constant"
                                                  : "symbol";
      snprintf(error_msg, sizeof(error_msg),
               "'%s' is a %s and cannot be assigned to",
               assignment->variable_name, symbol_type);
      type_checker_set_error_at_location(checker, declaration->location,
                                         error_msg);
      return 0;
    }

    if (var_symbol->is_immutable) {
      type_checker_set_error_at_location(
          checker, declaration->location,
          "'%s' is a constant and cannot be assigned to",
          assignment->variable_name);
      return 0;
    }

    // Infer the type of the assignment value
    Type *value_type = type_checker_infer_type(checker, assignment->value);
    if (!value_type) {
      if (!checker->has_error) {
        type_checker_set_error_at_location(
            checker, assignment->value->location,
            "Cannot infer type of assignment value");
      }
      return 0;
    }

    // Validate assignment compatibility
    if (!(var_symbol->type->kind == TYPE_POINTER &&
          type_checker_is_null_pointer_constant(assignment->value)) &&
        !type_checker_is_assignable(checker, var_symbol->type, value_type)) {
      type_checker_report_type_mismatch(checker, assignment->value->location,
                                        var_symbol->type->name,
                                        value_type->name);
      return 0;
    }

    if (checker->current_function && var_symbol->scope &&
        var_symbol->scope->type != SCOPE_GLOBAL) {
      type_checker_init_tracker_set_initialized(checker,
                                                assignment->variable_name);
      if (var_symbol->type && var_symbol->type->kind == TYPE_POINTER) {
        long long known_extent =
            type_checker_extract_known_buffer_extent(checker, assignment->value);
        long long known_alignment = type_checker_extract_known_pointer_alignment(
            checker, assignment->value);
        if (!type_checker_buffer_extent_set(checker, assignment->variable_name,
                                            known_extent, known_alignment)) {
          type_checker_set_error_at_location(
              checker, assignment->value->location,
              "Out of memory while tracking buffer extent for '%s'",
              assignment->variable_name);
          return 0;
        }
      }
    }

    return 1;
  }

  default:
    type_checker_set_error_at_location(
        checker, declaration->location,
        "Unsupported top-level construct in declaration context");
    return 0;
  }
}
