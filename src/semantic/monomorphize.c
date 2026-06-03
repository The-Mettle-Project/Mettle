#include "monomorphize.h"
#include "../common.h"
#include "../string_intern.h"
#include "../common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  char *name;
  ASTNode **methods;
  size_t method_count;
} TraitDef;

typedef struct {
  char *trait_name;
  char *for_type_name;
  ASTNode **methods;
  size_t method_count;
} TraitImpl;

typedef struct {
  char *generic_name;
  char **type_args;
  size_t type_arg_count;
  char *mangled_name;
  SourceLocation location;
  int emitted;
} Instantiation;

typedef struct {
  char *name;
  ASTNode *node;
  char **type_params;
  char **type_param_traits;
  size_t type_param_count;
  int is_struct; // 1 = struct, 0 = function
} GenericDef;

typedef struct {
  ErrorReporter *reporter;
  int had_error;
  TraitDef *traits;
  size_t trait_count;
  TraitImpl *impls;
  size_t impl_count;
  GenericDef *defs;
  size_t def_count;
  Instantiation *instances;
  size_t instance_count;
} MonoContext;

static char *substitute_type_string(const char *type_str, char **param_names,
                                    char **arg_names, size_t count,
                                    MonoContext *ctx);
static void substitute_types_in_ast(ASTNode *node, char **param_names,
                                    char **arg_names, size_t count,
                                    MonoContext *ctx);

static char *mangle_name(const char *base, char **type_args,
                           size_t type_arg_count) {
  size_t len = strlen(base);
  for (size_t i = 0; i < type_arg_count; i++) {
    len += 2 + strlen(type_args[i]) * 4;
  }
  len += 1;

  char *result = malloc(len + 64);
  if (!result)
    return NULL;

  size_t base_len = strlen(base);
  memcpy(result, base, base_len);
  size_t pos = base_len;

  for (size_t i = 0; i < type_arg_count; i++) {
    result[pos++] = '_';
    result[pos++] = '_';
    for (const char *p = type_args[i]; *p; p++) {
      if (*p == '*') {
        memcpy(result + pos, "_ptr", 4);
        pos += 4;
      } else if (*p == '<' || *p == '>' || *p == ',') {
        result[pos++] = '_';
      } else {
        result[pos++] = *p;
      }
    }
  }
  result[pos] = '\0';

  return result;
}

static void mono_report_error(MonoContext *ctx, SourceLocation location,
                              const char *message) {
  if (!ctx) {
    return;
  }

  ctx->had_error = 1;
  if (ctx->reporter) {
    error_reporter_add_error(ctx->reporter, ERROR_SEMANTIC, location, message);
  }
}

static void free_string_array(char **values, size_t count) {
  mettle_free_string_array(values, count);
}

static int mono_has_trait(MonoContext *ctx, const char *trait_name) {
  if (!ctx || !trait_name) {
    return 0;
  }

  for (size_t i = 0; i < ctx->trait_count; i++) {
    if (strcmp(ctx->traits[i].name, trait_name) == 0) {
      return 1;
    }
  }

  return 0;
}

static int mono_add_trait(MonoContext *ctx, const char *trait_name,
                          SourceLocation location) {
  if (!ctx || !trait_name) {
    return 0;
  }

  if (mono_has_trait(ctx, trait_name)) {
    char message[512];
    snprintf(message, sizeof(message), "Duplicate trait declaration '%s'",
             trait_name);
    mono_report_error(ctx, location, message);
    return 0;
  }

  TraitDef *grown =
      realloc(ctx->traits, (ctx->trait_count + 1) * sizeof(TraitDef));
  if (!grown) {
    mono_report_error(ctx, location,
                      "Failed to allocate storage for trait declaration");
    return 0;
  }

  ctx->traits = grown;
  ctx->traits[ctx->trait_count].name = strdup(trait_name);
  if (!ctx->traits[ctx->trait_count].name) {
    mono_report_error(ctx, location,
                      "Failed to allocate storage for trait declaration");
    return 0;
  }
  ctx->traits[ctx->trait_count].methods = NULL;
  ctx->traits[ctx->trait_count].method_count = 0;

  ctx->trait_count++;
  return 1;
}

static TraitDef *mono_find_trait(MonoContext *ctx, const char *trait_name) {
  if (!ctx || !trait_name) {
    return NULL;
  }

  for (size_t i = 0; i < ctx->trait_count; i++) {
    if (strcmp(ctx->traits[i].name, trait_name) == 0) {
      return &ctx->traits[i];
    }
  }

  return NULL;
}

static ASTNode *mono_find_function_method(ASTNode **methods,
                                          size_t method_count,
                                          const char *name) {
  if (!methods || !name) {
    return NULL;
  }

  for (size_t i = 0; i < method_count; i++) {
    ASTNode *method = methods[i];
    if (!method || method->type != AST_FUNCTION_DECLARATION) {
      continue;
    }
    FunctionDeclaration *decl = (FunctionDeclaration *)method->data;
    if (decl && decl->name && strcmp(decl->name, name) == 0) {
      return method;
    }
  }

  return NULL;
}

static int mono_type_implements_trait(MonoContext *ctx, const char *trait_name,
                                      const char *type_name) {
  if (!ctx || !trait_name || !type_name) {
    return 0;
  }

  for (size_t i = 0; i < ctx->impl_count; i++) {
    if (strcmp(ctx->impls[i].trait_name, trait_name) == 0 &&
        strcmp(ctx->impls[i].for_type_name, type_name) == 0) {
      return 1;
    }
  }

  return 0;
}

static char **mono_split_bounds(const char *bounds, size_t *out_count) {
  char **items = NULL;
  size_t count = 0;
  const char *start = bounds;

  if (out_count) {
    *out_count = 0;
  }
  if (!bounds || !out_count) {
    return NULL;
  }

  while (*start) {
    const char *end = strchr(start, '+');
    size_t len = end ? (size_t)(end - start) : strlen(start);

    while (len > 0 && (*start == ' ' || *start == '\t')) {
      start++;
      len--;
    }
    while (len > 0 && (start[len - 1] == ' ' || start[len - 1] == '\t')) {
      len--;
    }

    if (len > 0) {
      char **grown = realloc(items, (count + 1) * sizeof(char *));
      if (!grown) {
        free_string_array(items, count);
        return NULL;
      }
      items = grown;
      items[count] = malloc(len + 1);
      if (!items[count]) {
        free_string_array(items, count);
        return NULL;
      }
      memcpy(items[count], start, len);
      items[count][len] = '\0';
      count++;
    }

    if (!end) {
      break;
    }
    start = end + 1;
  }

  *out_count = count;
  return items;
}

static int mono_validate_bound_traits(MonoContext *ctx, const char *bounds,
                                      const char *generic_name,
                                      SourceLocation location) {
  char **traits = NULL;
  size_t trait_count = 0;
  int ok = 1;

  if (!bounds) {
    return 1;
  }

  traits = mono_split_bounds(bounds, &trait_count);
  if (!traits && trait_count == 0) {
    mono_report_error(ctx, location,
                      "Failed to allocate storage for generic bounds");
    return 0;
  }

  for (size_t i = 0; i < trait_count; i++) {
    if (!mono_has_trait(ctx, traits[i])) {
      char message[512];
      snprintf(message, sizeof(message),
               "Unknown trait '%s' in generic bound on '%s'", traits[i],
               generic_name);
      mono_report_error(ctx, location, message);
      ok = 0;
      break;
    }
  }

  free_string_array(traits, trait_count);
  return ok;
}

static int mono_type_satisfies_bounds(MonoContext *ctx, GenericDef *def,
                                      Instantiation *inst,
                                      const char *bounds, size_t arg_index) {
  char **traits = NULL;
  size_t trait_count = 0;
  int ok = 1;

  if (!bounds) {
    return 1;
  }

  traits = mono_split_bounds(bounds, &trait_count);
  if (!traits && trait_count == 0) {
    mono_report_error(ctx, inst->location,
                      "Failed to allocate storage for generic bounds");
    return 0;
  }

  for (size_t i = 0; i < trait_count; i++) {
    if (!mono_has_trait(ctx, traits[i])) {
      char message[512];
      snprintf(message, sizeof(message),
               "Generic '%s' requires unknown trait '%s'", def->name,
               traits[i]);
      mono_report_error(ctx, inst->location, message);
      ok = 0;
      break;
    }

    if (!mono_type_implements_trait(ctx, traits[i], inst->type_args[arg_index])) {
      char message[512];
      snprintf(message, sizeof(message),
               "Type '%s' does not implement trait '%s' required by '%s'",
               inst->type_args[arg_index], traits[i], def->name);
      mono_report_error(ctx, inst->location, message);
      ok = 0;
      break;
    }
  }

  free_string_array(traits, trait_count);
  return ok;
}

static int mono_add_impl(MonoContext *ctx, const char *trait_name,
                         const char *for_type_name, SourceLocation location) {
  if (!ctx || !trait_name || !for_type_name) {
    return 0;
  }

  if (!mono_has_trait(ctx, trait_name)) {
    char message[512];
    snprintf(message, sizeof(message),
             "Unknown trait '%s' in impl declaration", trait_name);
    mono_report_error(ctx, location, message);
    return 0;
  }

  if (mono_type_implements_trait(ctx, trait_name, for_type_name)) {
    char message[512];
    snprintf(message, sizeof(message), "Duplicate impl '%s for %s'", trait_name,
             for_type_name);
    mono_report_error(ctx, location, message);
    return 0;
  }

  TraitImpl *grown =
      realloc(ctx->impls, (ctx->impl_count + 1) * sizeof(TraitImpl));
  if (!grown) {
    mono_report_error(ctx, location,
                      "Failed to allocate storage for impl declaration");
    return 0;
  }

  ctx->impls = grown;
  ctx->impls[ctx->impl_count].trait_name = strdup(trait_name);
  ctx->impls[ctx->impl_count].for_type_name = strdup(for_type_name);
  ctx->impls[ctx->impl_count].methods = NULL;
  ctx->impls[ctx->impl_count].method_count = 0;
  if (!ctx->impls[ctx->impl_count].trait_name ||
      !ctx->impls[ctx->impl_count].for_type_name) {
    free(ctx->impls[ctx->impl_count].trait_name);
    free(ctx->impls[ctx->impl_count].for_type_name);
    mono_report_error(ctx, location,
                      "Failed to allocate storage for impl declaration");
    return 0;
  }

  ctx->impl_count++;
  return 1;
}

static char *mono_sanitize_component(const char *value) {
  size_t len = value ? strlen(value) : 0;
  char *result = malloc(len + 1);
  if (!result) {
    return NULL;
  }

  for (size_t i = 0; i < len; i++) {
    char c = value[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_') {
      result[i] = c;
    } else {
      result[i] = '_';
    }
  }
  result[len] = '\0';
  return result;
}

static char *mono_mangle_impl_method_name(const char *trait_name,
                                          const char *for_type_name,
                                          const char *method_name) {
  char *trait_part = mono_sanitize_component(trait_name);
  char *type_part = mono_sanitize_component(for_type_name);
  char *method_part = mono_sanitize_component(method_name);
  char *result = NULL;
  size_t len = 0;

  if (!trait_part || !type_part || !method_part) {
    free(trait_part);
    free(type_part);
    free(method_part);
    return NULL;
  }

  len = strlen("__trait_") + strlen(trait_part) + 2 + strlen(type_part) + 2 +
        strlen(method_part) + 1;
  result = malloc(len);
  if (result) {
    snprintf(result, len, "__trait_%s__%s__%s", trait_part, type_part,
             method_part);
  }

  free(trait_part);
  free(type_part);
  free(method_part);
  return result;
}

static char *mono_substitute_self_type(const char *type_name,
                                       const char *for_type_name,
                                       MonoContext *ctx) {
  char *param = "Self";
  char *arg = (char *)for_type_name;
  return substitute_type_string(type_name, &param, &arg, 1, ctx);
}

static int mono_type_names_equal_with_self(const char *trait_type,
                                           const char *impl_type,
                                           const char *for_type_name,
                                           MonoContext *ctx) {
  char *expected = mono_substitute_self_type(trait_type, for_type_name, ctx);
  char *actual = mono_substitute_self_type(impl_type, for_type_name, ctx);
  int equal = 0;

  if (!expected || !actual) {
    free(expected);
    free(actual);
    return 0;
  }

  equal = strcmp(expected, actual) == 0;
  free(expected);
  free(actual);
  return equal;
}

static int mono_validate_impl_methods(MonoContext *ctx, TraitImpl *impl,
                                      SourceLocation location) {
  TraitDef *trait = NULL;

  if (!ctx || !impl) {
    return 0;
  }

  trait = mono_find_trait(ctx, impl->trait_name);
  if (!trait) {
    return 0;
  }

  for (size_t i = 0; i < trait->method_count; i++) {
    ASTNode *trait_method = trait->methods[i];
    FunctionDeclaration *trait_fn =
        trait_method ? (FunctionDeclaration *)trait_method->data : NULL;
    ASTNode *impl_method = NULL;
    FunctionDeclaration *impl_fn = NULL;

    if (!trait_fn || !trait_fn->name) {
      continue;
    }

    impl_method =
        mono_find_function_method(impl->methods, impl->method_count,
                                  trait_fn->name);
    if (!impl_method) {
      char message[512];
      snprintf(message, sizeof(message),
               "Impl '%s for %s' is missing trait method '%s'",
               impl->trait_name, impl->for_type_name, trait_fn->name);
      mono_report_error(ctx, location, message);
      return 0;
    }

    impl_fn = (FunctionDeclaration *)impl_method->data;
    if (!impl_fn || impl_fn->parameter_count != trait_fn->parameter_count) {
      char message[512];
      snprintf(message, sizeof(message),
               "Impl method '%s' has a signature that does not match trait '%s'",
               trait_fn->name, impl->trait_name);
      mono_report_error(ctx, location, message);
      return 0;
    }

    for (size_t j = 0; j < trait_fn->parameter_count; j++) {
      if (!mono_type_names_equal_with_self(trait_fn->parameter_types[j],
                                           impl_fn->parameter_types[j],
                                           impl->for_type_name, ctx)) {
        char message[512];
        snprintf(message, sizeof(message),
                 "Impl method '%s' parameter %llu does not match trait '%s'",
                 trait_fn->name, (unsigned long long)(j + 1),
                 impl->trait_name);
        mono_report_error(ctx, location, message);
        return 0;
      }
    }

    if ((trait_fn->return_type || impl_fn->return_type) &&
        !mono_type_names_equal_with_self(trait_fn->return_type
                                             ? trait_fn->return_type
                                             : "void",
                                         impl_fn->return_type
                                             ? impl_fn->return_type
                                             : "void",
                                         impl->for_type_name, ctx)) {
      char message[512];
      snprintf(message, sizeof(message),
               "Impl method '%s' return type does not match trait '%s'",
               trait_fn->name, impl->trait_name);
      mono_report_error(ctx, location, message);
      return 0;
    }
  }

  return 1;
}

static ASTNode *mono_create_impl_method_function(MonoContext *ctx,
                                                TraitImpl *impl,
                                                ASTNode *method) {
  ASTNode *clone = NULL;
  FunctionDeclaration *fn = NULL;
  char *mangled = NULL;

  if (!ctx || !impl || !method || method->type != AST_FUNCTION_DECLARATION) {
    return NULL;
  }

  clone = ast_clone_node(method);
  if (!clone) {
    return NULL;
  }

  fn = (FunctionDeclaration *)clone->data;
  if (!fn || !fn->name) {
    ast_destroy_node(clone);
    return NULL;
  }

  mangled = mono_mangle_impl_method_name(impl->trait_name, impl->for_type_name,
                                         fn->name);
  if (!mangled) {
    ast_destroy_node(clone);
    return NULL;
  }

  mettle_free_string(fn->name);
  fn->name = mangled;
  fn->is_exported = 0;

  for (size_t i = 0; i < fn->parameter_count; i++) {
    char *new_type =
        mono_substitute_self_type(fn->parameter_types[i], impl->for_type_name,
                                  ctx);
    if (new_type) {
      mettle_free_string(fn->parameter_types[i]);
      fn->parameter_types[i] = new_type;
    }
  }

  if (fn->return_type) {
    char *new_type =
        mono_substitute_self_type(fn->return_type, impl->for_type_name, ctx);
    if (new_type) {
      mettle_free_string(fn->return_type);
      fn->return_type = new_type;
    }
  }

  if (fn->body) {
    char *param = "Self";
    char *arg = impl->for_type_name;
    substitute_types_in_ast(fn->body, &param, &arg, 1, ctx);
  }

  return clone;
}

static int mono_add_instantiation(MonoContext *ctx, const char *generic_name,
                                  char **type_args, size_t type_arg_count,
                                  SourceLocation location) {
  char *mangled = NULL;
  int found = 0;
  Instantiation *inst = NULL;

  if (!ctx || !generic_name || !type_args) {
    return 0;
  }

  mangled = mangle_name(generic_name, type_args, type_arg_count);
  if (!mangled) {
    mono_report_error(ctx, location,
                      "Failed to allocate storage for generic instantiation");
    return 0;
  }

  for (size_t i = 0; i < ctx->instance_count; i++) {
    if (strcmp(ctx->instances[i].mangled_name, mangled) == 0) {
      found = 1;
      break;
    }
  }

  if (found) {
    free(mangled);
    return 1;
  }

  Instantiation *grown = realloc(ctx->instances,
                                 (ctx->instance_count + 1) *
                                     sizeof(Instantiation));
  if (!grown) {
    free(mangled);
    mono_report_error(ctx, location,
                      "Failed to allocate storage for generic instantiation");
    return 0;
  }

  ctx->instances = grown;
  inst = &ctx->instances[ctx->instance_count];
  inst->generic_name = strdup(generic_name);
  inst->type_arg_count = type_arg_count;
  inst->type_args = malloc(type_arg_count * sizeof(char *));
  inst->mangled_name = mangled;
  inst->location = location;
  inst->emitted = 0;

  if (!inst->generic_name || !inst->type_args) {
    free(inst->generic_name);
    free(inst->type_args);
    free(inst->mangled_name);
    mono_report_error(ctx, location,
                      "Failed to allocate storage for generic instantiation");
    return 0;
  }

  for (size_t i = 0; i < type_arg_count; i++) {
    inst->type_args[i] = strdup(type_args[i]);
    if (!inst->type_args[i]) {
      free_string_array(inst->type_args, i);
      free(inst->generic_name);
      free(inst->mangled_name);
      mono_report_error(ctx, location,
                        "Failed to allocate storage for generic instantiation");
      return 0;
    }
  }

  ctx->instance_count++;
  return 1;
}

static int parse_generic_type_name(const char *type_str, char **out_base,
                                   char ***out_args, size_t *out_arg_count) {
  const char *lt = strchr(type_str, '<');
  if (!lt)
    return 0;

  size_t base_len = (size_t)(lt - type_str);
  *out_base = malloc(base_len + 1);
  memcpy(*out_base, type_str, base_len);
  (*out_base)[base_len] = '\0';

  const char *start = lt + 1;
  const char *end = type_str + strlen(type_str);

  // Find matching '>'
  const char *gt = NULL;
  int depth = 1;
  for (const char *p = start; *p; p++) {
    if (*p == '<')
      depth++;
    else if (*p == '>') {
      depth--;
      if (depth == 0) {
        gt = p;
        break;
      }
    }
  }

  if (!gt) {
    free(*out_base);
    *out_base = NULL;
    return 0;
  }

  // Parse comma-separated args between start and gt
  char **args = NULL;
  size_t count = 0;
  const char *p = start;

  while (p < gt) {
    while (p < gt && *p == ' ')
      p++;

    const char *arg_start = p;
    int inner_depth = 0;
    while (p < gt) {
      if (*p == '<')
        inner_depth++;
      else if (*p == '>')
        inner_depth--;
      else if (*p == ',' && inner_depth == 0)
        break;
      p++;
    }

    const char *arg_end = p;
    while (arg_end > arg_start && *(arg_end - 1) == ' ')
      arg_end--;

    size_t arg_len = (size_t)(arg_end - arg_start);
    if (arg_len > 0) {
      args = realloc(args, (count + 1) * sizeof(char *));
      args[count] = malloc(arg_len + 1);
      memcpy(args[count], arg_start, arg_len);
      args[count][arg_len] = '\0';
      count++;
    }

    if (p < gt && *p == ',')
      p++;
  }

  // Check for pointer suffix after '>' (e.g., "List<int32>*")
  const char *suffix = gt + 1;
  if (suffix < end && *suffix != '\0') {
    // There's a suffix like "*" — we need to incorporate it
    // The base stays the same, but we need the caller to handle the suffix
    // For simplicity, append the suffix to the last arg? No — the suffix
    // applies to the whole generic type. We'll handle this by including
    // the suffix in the base name replacement later.
  }

  *out_args = args;
  *out_arg_count = count;
  return 1;
}

static void record_generic_type_use(MonoContext *ctx, const char *type_name,
                                    SourceLocation location) {
  char *base = NULL;
  char **args = NULL;
  size_t arg_count = 0;

  if (!ctx || !type_name) {
    return;
  }

  if (!parse_generic_type_name(type_name, &base, &args, &arg_count)) {
    return;
  }

  mono_add_instantiation(ctx, base, args, arg_count, location);
  free(base);
  free_string_array(args, arg_count);
}

static char *substitute_type_string(const char *type_str, char **param_names,
                                    char **arg_names, size_t count,
                                    MonoContext *ctx);

static char *substitute_type_string(const char *type_str, char **param_names,
                                    char **arg_names, size_t count,
                                    MonoContext *ctx) {
  if (!type_str)
    return NULL;

  size_t ts_len = strlen(type_str);

  // Strip trailing pointer stars and array suffixes to get the core type
  size_t core_len = ts_len;
  size_t ptr_count = 0;
  char *array_suffix = NULL;

  // Check for array suffix first: "type[N]"
  const char *lbr = NULL;
  int depth = 0;
  for (size_t i = 0; i < ts_len; i++) {
    if (type_str[i] == '<')
      depth++;
    else if (type_str[i] == '>')
      depth--;
    else if (type_str[i] == '[' && depth == 0) {
      lbr = type_str + i;
      break;
    }
  }

  if (lbr) {
    array_suffix = strdup(lbr);
    core_len = (size_t)(lbr - type_str);
  }

  // Strip trailing '*' from core
  while (core_len > 0 && type_str[core_len - 1] == '*') {
    ptr_count++;
    core_len--;
  }

  char *core = malloc(core_len + 1);
  memcpy(core, type_str, core_len);
  core[core_len] = '\0';

  // Check if core is a type parameter
  for (size_t i = 0; i < count; i++) {
    if (strcmp(core, param_names[i]) == 0) {
      free(core);
      size_t result_len = strlen(arg_names[i]) + ptr_count +
                          (array_suffix ? strlen(array_suffix) : 0) + 1;
      char *result = malloc(result_len);
      strcpy(result, arg_names[i]);
      for (size_t j = 0; j < ptr_count; j++)
        strcat(result, "*");
      if (array_suffix) {
        strcat(result, array_suffix);
        free(array_suffix);
      }
      return result;
    }
  }

  // Check if core is a generic type instantiation like "List<T>"
  char *gen_base = NULL;
  char **gen_args = NULL;
  size_t gen_arg_count = 0;
  if (parse_generic_type_name(core, &gen_base, &gen_args, &gen_arg_count)) {
    // Substitute type params within the generic args
    char **subst_args = malloc(gen_arg_count * sizeof(char *));
    for (size_t i = 0; i < gen_arg_count; i++) {
      subst_args[i] =
          substitute_type_string(gen_args[i], param_names, arg_names, count, ctx);
    }

    // Generate mangled name for this instantiation
    char *mangled = mangle_name(gen_base, subst_args, gen_arg_count);

    // Record this as an instantiation if not already recorded
    if (ctx) {
      int found = 0;
      for (size_t i = 0; i < ctx->instance_count; i++) {
        if (strcmp(ctx->instances[i].mangled_name, mangled) == 0) {
          found = 1;
          break;
        }
      }
      if (!found) {
        SourceLocation internal_location = {0, 0, NULL};
        mono_add_instantiation(ctx, gen_base, subst_args, gen_arg_count,
                               internal_location);
      }
    }

    size_t result_len =
        strlen(mangled) + ptr_count +
        (array_suffix ? strlen(array_suffix) : 0) + 1;
    char *result = malloc(result_len);
    strcpy(result, mangled);
    for (size_t j = 0; j < ptr_count; j++)
      strcat(result, "*");
    if (array_suffix) {
      strcat(result, array_suffix);
      free(array_suffix);
    }

    free(mangled);
    free(gen_base);
    for (size_t i = 0; i < gen_arg_count; i++) {
      free(gen_args[i]);
      free(subst_args[i]);
    }
    free(gen_args);
    free(subst_args);
    free(core);
    return result;
  }

  free(core);
  free(array_suffix);

  return strdup(type_str);
}

static void substitute_types_in_ast(ASTNode *node, char **param_names,
                                    char **arg_names, size_t count,
                                    MonoContext *ctx) {
  if (!node)
    return;

  switch (node->type) {
  case AST_VAR_DECLARATION: {
    VarDeclaration *vd = (VarDeclaration *)node->data;
    if (vd && vd->type_name) {
      char *new_type =
          substitute_type_string(vd->type_name, param_names, arg_names, count, ctx);
      if (new_type) {
        mettle_free_string(vd->type_name);
        vd->type_name = new_type;
      }
    }
    if (vd && vd->initializer) {
      substitute_types_in_ast(vd->initializer, param_names, arg_names, count, ctx);
    }
    break;
  }
  case AST_NEW_EXPRESSION: {
    NewExpression *ne = (NewExpression *)node->data;
    if (ne && ne->type_name) {
      char *new_type =
          substitute_type_string(ne->type_name, param_names, arg_names, count, ctx);
      if (new_type) {
        mettle_free_string(ne->type_name);
        ne->type_name = new_type;
      }
    }
    break;
  }
  case AST_FUNCTION_CALL: {
    CallExpression *ce = (CallExpression *)node->data;
    if (ce) {
      // Substitute type args in generic function calls
      if (ce->type_arg_count > 0 && ce->type_args) {
        for (size_t i = 0; i < ce->type_arg_count; i++) {
          char *new_arg = substitute_type_string(ce->type_args[i], param_names,
                                                 arg_names, count, ctx);
          if (new_arg) {
            mettle_free_string(ce->type_args[i]);
            ce->type_args[i] = new_arg;
          }
        }
      }
      for (size_t i = 0; i < ce->argument_count; i++) {
        substitute_types_in_ast(ce->arguments[i], param_names, arg_names, count,
                                ctx);
      }
      if (ce->object) {
        substitute_types_in_ast(ce->object, param_names, arg_names, count, ctx);
      }
    }
    break;
  }
  case AST_PROGRAM: {
    Program *prog = (Program *)node->data;
    if (prog) {
      for (size_t i = 0; i < prog->declaration_count; i++) {
        substitute_types_in_ast(prog->declarations[i], param_names, arg_names,
                                count, ctx);
      }
    }
    break;
  }
  case AST_RETURN_STATEMENT: {
    ReturnStatement *rs = (ReturnStatement *)node->data;
    if (rs && rs->value) {
      substitute_types_in_ast(rs->value, param_names, arg_names, count, ctx);
    }
    break;
  }
  case AST_ASSIGNMENT: {
    Assignment *as = (Assignment *)node->data;
    if (as) {
      if (as->value)
        substitute_types_in_ast(as->value, param_names, arg_names, count, ctx);
      if (as->target)
        substitute_types_in_ast(as->target, param_names, arg_names, count, ctx);
    }
    break;
  }
  case AST_IF_STATEMENT: {
    IfStatement *is = (IfStatement *)node->data;
    if (is) {
      substitute_types_in_ast(is->condition, param_names, arg_names, count, ctx);
      substitute_types_in_ast(is->then_branch, param_names, arg_names, count, ctx);
      for (size_t i = 0; i < is->else_if_count; i++) {
        substitute_types_in_ast(is->else_ifs[i].condition, param_names,
                                arg_names, count, ctx);
        substitute_types_in_ast(is->else_ifs[i].body, param_names, arg_names,
                                count, ctx);
      }
      if (is->else_branch)
        substitute_types_in_ast(is->else_branch, param_names, arg_names, count,
                                ctx);
    }
    break;
  }
  case AST_WHILE_STATEMENT: {
    WhileStatement *ws = (WhileStatement *)node->data;
    if (ws) {
      substitute_types_in_ast(ws->condition, param_names, arg_names, count, ctx);
      substitute_types_in_ast(ws->body, param_names, arg_names, count, ctx);
    }
    break;
  }
  case AST_FOR_STATEMENT: {
    ForStatement *fs = (ForStatement *)node->data;
    if (fs) {
      if (fs->initializer)
        substitute_types_in_ast(fs->initializer, param_names, arg_names, count,
                                ctx);
      if (fs->condition)
        substitute_types_in_ast(fs->condition, param_names, arg_names, count,
                                ctx);
      if (fs->increment)
        substitute_types_in_ast(fs->increment, param_names, arg_names, count,
                                ctx);
      if (fs->body)
        substitute_types_in_ast(fs->body, param_names, arg_names, count, ctx);
    }
    break;
  }
  case AST_SWITCH_STATEMENT: {
    SwitchStatement *ss = (SwitchStatement *)node->data;
    if (ss) {
      substitute_types_in_ast(ss->expression, param_names, arg_names, count,
                              ctx);
      for (size_t i = 0; i < ss->case_count; i++) {
        substitute_types_in_ast(ss->cases[i], param_names, arg_names, count,
                                ctx);
      }
    }
    break;
  }
  case AST_CASE_CLAUSE: {
    CaseClause *cc = (CaseClause *)node->data;
    if (cc) {
      if (cc->value)
        substitute_types_in_ast(cc->value, param_names, arg_names, count, ctx);
      if (cc->body)
        substitute_types_in_ast(cc->body, param_names, arg_names, count, ctx);
    }
    break;
  }
  case AST_BINARY_EXPRESSION: {
    BinaryExpression *be = (BinaryExpression *)node->data;
    if (be) {
      substitute_types_in_ast(be->left, param_names, arg_names, count, ctx);
      substitute_types_in_ast(be->right, param_names, arg_names, count, ctx);
    }
    break;
  }
  case AST_UNARY_EXPRESSION: {
    UnaryExpression *ue = (UnaryExpression *)node->data;
    if (ue && ue->operand) {
      substitute_types_in_ast(ue->operand, param_names, arg_names, count, ctx);
    }
    break;
  }
  case AST_MEMBER_ACCESS: {
    MemberAccess *ma = (MemberAccess *)node->data;
    if (ma && ma->object) {
      substitute_types_in_ast(ma->object, param_names, arg_names, count, ctx);
    }
    break;
  }
  case AST_INDEX_EXPRESSION: {
    ArrayIndexExpression *ie = (ArrayIndexExpression *)node->data;
    if (ie) {
      if (ie->array)
        substitute_types_in_ast(ie->array, param_names, arg_names, count, ctx);
      if (ie->index)
        substitute_types_in_ast(ie->index, param_names, arg_names, count, ctx);
    }
    break;
  }
  case AST_DEFER_STATEMENT:
  case AST_ERRDEFER_STATEMENT: {
    DeferStatement *ds = (DeferStatement *)node->data;
    if (ds && ds->statement)
      substitute_types_in_ast(ds->statement, param_names, arg_names, count, ctx);
    break;
  }
  default:
    break;
  }
}

static void collect_generic_defs(ASTNode *program, MonoContext *ctx) {
  Program *prog = (Program *)program->data;
  if (!prog)
    return;

  for (size_t i = 0; i < prog->declaration_count; i++) {
    ASTNode *decl = prog->declarations[i];
    if (decl && decl->type == AST_TRAIT_DECLARATION) {
      TraitDeclaration *td = (TraitDeclaration *)decl->data;
      if (td && td->name) {
        if (mono_add_trait(ctx, td->name, decl->location)) {
          TraitDef *trait = mono_find_trait(ctx, td->name);
          if (trait) {
            trait->methods = td->methods;
            trait->method_count = td->method_count;
          }
        }
      }
    }
  }

  for (size_t i = 0; i < prog->declaration_count; i++) {
    ASTNode *decl = prog->declarations[i];
    if (!decl)
      continue;

    if (decl->type == AST_IMPL_DECLARATION) {
      ImplDeclaration *impl = (ImplDeclaration *)decl->data;
      if (impl && impl->trait_name && impl->for_type_name) {
        size_t old_count = ctx->impl_count;
        if (mono_add_impl(ctx, impl->trait_name, impl->for_type_name,
                          decl->location) &&
            ctx->impl_count > old_count) {
          ctx->impls[ctx->impl_count - 1].methods = impl->methods;
          ctx->impls[ctx->impl_count - 1].method_count = impl->method_count;
          mono_validate_impl_methods(ctx, &ctx->impls[ctx->impl_count - 1],
                                     decl->location);
        }
      }
    } else if (decl->type == AST_STRUCT_DECLARATION) {
      StructDeclaration *sd = (StructDeclaration *)decl->data;
      if (sd && sd->type_param_count > 0) {
        GenericDef *new_defs =
            realloc(ctx->defs, (ctx->def_count + 1) * sizeof(GenericDef));
        if (!new_defs) {
          mono_report_error(ctx, decl->location,
                            "Out of memory collecting generic definitions");
          return;
        }
        ctx->defs = new_defs;
        GenericDef *def = &ctx->defs[ctx->def_count];
        def->name = strdup(sd->name);
        def->node = decl;
        def->type_param_count = sd->type_param_count;
        def->type_params = malloc(sd->type_param_count * sizeof(char *));
        def->type_param_traits = malloc(sd->type_param_count * sizeof(char *));
        for (size_t j = 0; j < sd->type_param_count; j++) {
          def->type_params[j] = strdup(sd->type_params[j]);
          def->type_param_traits[j] =
              sd->type_param_traits && sd->type_param_traits[j]
                  ? strdup(sd->type_param_traits[j])
                  : NULL;
          if (!mono_validate_bound_traits(ctx, def->type_param_traits[j],
                                          sd->name, decl->location)) {
            continue;
          }
        }
        def->is_struct = 1;
        ctx->def_count++;
      }
    } else if (decl->type == AST_FUNCTION_DECLARATION) {
      FunctionDeclaration *fd = (FunctionDeclaration *)decl->data;
      if (fd && fd->type_param_count > 0) {
        GenericDef *new_defs =
            realloc(ctx->defs, (ctx->def_count + 1) * sizeof(GenericDef));
        if (!new_defs) {
          mono_report_error(ctx, decl->location,
                            "Out of memory collecting generic definitions");
          return;
        }
        ctx->defs = new_defs;
        GenericDef *def = &ctx->defs[ctx->def_count];
        def->name = strdup(fd->name);
        def->node = decl;
        def->type_param_count = fd->type_param_count;
        def->type_params = malloc(fd->type_param_count * sizeof(char *));
        def->type_param_traits = malloc(fd->type_param_count * sizeof(char *));
        for (size_t j = 0; j < fd->type_param_count; j++) {
          def->type_params[j] = strdup(fd->type_params[j]);
          def->type_param_traits[j] =
              fd->type_param_traits && fd->type_param_traits[j]
                  ? strdup(fd->type_param_traits[j])
                  : NULL;
          if (!mono_validate_bound_traits(ctx, def->type_param_traits[j],
                                          fd->name, decl->location)) {
            continue;
          }
        }
        def->is_struct = 0;
        ctx->def_count++;
      }
    }
  }
}

static void collect_type_instantiations(ASTNode *node, MonoContext *ctx) {
  if (!node)
    return;

  switch (node->type) {
  case AST_VAR_DECLARATION: {
    VarDeclaration *vd = (VarDeclaration *)node->data;
    if (vd && vd->type_name) {
      record_generic_type_use(ctx, vd->type_name, node->location);
    }
    if (vd && vd->initializer)
      collect_type_instantiations(vd->initializer, ctx);
    break;
  }
  case AST_NEW_EXPRESSION: {
    NewExpression *ne = (NewExpression *)node->data;
    if (ne && ne->type_name) {
      record_generic_type_use(ctx, ne->type_name, node->location);
    }
    break;
  }
  case AST_FUNCTION_CALL: {
    CallExpression *ce = (CallExpression *)node->data;
    if (ce && ce->type_arg_count > 0 && ce->function_name) {
      mono_add_instantiation(ctx, ce->function_name, ce->type_args,
                             ce->type_arg_count, node->location);
    }
    if (ce) {
      for (size_t i = 0; i < ce->argument_count; i++)
        collect_type_instantiations(ce->arguments[i], ctx);
      if (ce->object)
        collect_type_instantiations(ce->object, ctx);
    }
    break;
  }
  case AST_FUNCTION_DECLARATION: {
    FunctionDeclaration *fd = (FunctionDeclaration *)node->data;
    if (fd && fd->type_param_count == 0 && fd->body)
      collect_type_instantiations(fd->body, ctx);
    // Also check parameter types for generic type uses
    if (fd && fd->type_param_count == 0) {
      for (size_t i = 0; i < fd->parameter_count; i++) {
        if (fd->parameter_types[i]) {
          record_generic_type_use(ctx, fd->parameter_types[i], node->location);
        }
      }
      if (fd->return_type) {
        record_generic_type_use(ctx, fd->return_type, node->location);
      }
    }
    break;
  }
  case AST_PROGRAM: {
    Program *prog = (Program *)node->data;
    if (prog) {
      for (size_t i = 0; i < prog->declaration_count; i++)
        collect_type_instantiations(prog->declarations[i], ctx);
    }
    break;
  }
  case AST_RETURN_STATEMENT: {
    ReturnStatement *rs = (ReturnStatement *)node->data;
    if (rs && rs->value)
      collect_type_instantiations(rs->value, ctx);
    break;
  }
  case AST_ASSIGNMENT: {
    Assignment *as = (Assignment *)node->data;
    if (as) {
      if (as->value)
        collect_type_instantiations(as->value, ctx);
      if (as->target)
        collect_type_instantiations(as->target, ctx);
    }
    break;
  }
  case AST_IF_STATEMENT: {
    IfStatement *is_stmt = (IfStatement *)node->data;
    if (is_stmt) {
      collect_type_instantiations(is_stmt->condition, ctx);
      collect_type_instantiations(is_stmt->then_branch, ctx);
      for (size_t i = 0; i < is_stmt->else_if_count; i++) {
        collect_type_instantiations(is_stmt->else_ifs[i].condition, ctx);
        collect_type_instantiations(is_stmt->else_ifs[i].body, ctx);
      }
      if (is_stmt->else_branch)
        collect_type_instantiations(is_stmt->else_branch, ctx);
    }
    break;
  }
  case AST_WHILE_STATEMENT: {
    WhileStatement *ws = (WhileStatement *)node->data;
    if (ws) {
      collect_type_instantiations(ws->condition, ctx);
      collect_type_instantiations(ws->body, ctx);
    }
    break;
  }
  case AST_FOR_STATEMENT: {
    ForStatement *fs = (ForStatement *)node->data;
    if (fs) {
      if (fs->initializer)
        collect_type_instantiations(fs->initializer, ctx);
      if (fs->condition)
        collect_type_instantiations(fs->condition, ctx);
      if (fs->increment)
        collect_type_instantiations(fs->increment, ctx);
      if (fs->body)
        collect_type_instantiations(fs->body, ctx);
    }
    break;
  }
  case AST_SWITCH_STATEMENT: {
    SwitchStatement *ss = (SwitchStatement *)node->data;
    if (ss) {
      collect_type_instantiations(ss->expression, ctx);
      for (size_t i = 0; i < ss->case_count; i++)
        collect_type_instantiations(ss->cases[i], ctx);
    }
    break;
  }
  case AST_CASE_CLAUSE: {
    CaseClause *cc = (CaseClause *)node->data;
    if (cc) {
      if (cc->value)
        collect_type_instantiations(cc->value, ctx);
      if (cc->body)
        collect_type_instantiations(cc->body, ctx);
    }
    break;
  }
  case AST_BINARY_EXPRESSION: {
    BinaryExpression *be = (BinaryExpression *)node->data;
    if (be) {
      collect_type_instantiations(be->left, ctx);
      collect_type_instantiations(be->right, ctx);
    }
    break;
  }
  case AST_UNARY_EXPRESSION: {
    UnaryExpression *ue = (UnaryExpression *)node->data;
    if (ue && ue->operand)
      collect_type_instantiations(ue->operand, ctx);
    break;
  }
  case AST_MEMBER_ACCESS: {
    MemberAccess *ma = (MemberAccess *)node->data;
    if (ma && ma->object)
      collect_type_instantiations(ma->object, ctx);
    break;
  }
  case AST_INDEX_EXPRESSION: {
    ArrayIndexExpression *ie = (ArrayIndexExpression *)node->data;
    if (ie) {
      if (ie->array)
        collect_type_instantiations(ie->array, ctx);
      if (ie->index)
        collect_type_instantiations(ie->index, ctx);
    }
    break;
  }
  case AST_DEFER_STATEMENT:
  case AST_ERRDEFER_STATEMENT: {
    DeferStatement *ds = (DeferStatement *)node->data;
    if (ds && ds->statement)
      collect_type_instantiations(ds->statement, ctx);
    break;
  }
  default:
    break;
  }
}

static void rewrite_generic_type_name_in_place(char **slot,
                                               const char **type_params,
                                               const char **concrete_types,
                                               size_t type_param_count) {
  char *type_str = *slot;
  size_t len = strlen(type_str);
  size_t ptr_count = 0;
  char *base = NULL;
  char **args = NULL;
  size_t arg_count = 0;

  (void)type_params;
  (void)concrete_types;
  (void)type_param_count;

  while (len > 0 && type_str[len - 1] == '*') {
    ptr_count++;
    len--;
  }

  char *core = malloc(len + 1);
  memcpy(core, type_str, len);
  core[len] = '\0';

  if (parse_generic_type_name(core, &base, &args, &arg_count)) {
    char *mangled = mangle_name(base, args, arg_count);
    size_t new_len = strlen(mangled) + ptr_count + 1;
    char *new_type = malloc(new_len);
    strcpy(new_type, mangled);
    for (size_t i = 0; i < ptr_count; i++)
      strcat(new_type, "*");
    mettle_free_string(*slot);
    *slot = new_type;
    free(mangled);
    free(base);
    for (size_t i = 0; i < arg_count; i++)
      free(args[i]);
    free(args);
  }
  free(core);
}

static void rewrite_generic_references(ASTNode *node, MonoContext *ctx) {
  if (!node)
    return;

  switch (node->type) {
  case AST_VAR_DECLARATION: {
    VarDeclaration *vd = (VarDeclaration *)node->data;
    if (vd && vd->type_name) {
      rewrite_generic_type_name_in_place(&vd->type_name, NULL, NULL, 0);
    }
    if (vd && vd->initializer)
      rewrite_generic_references(vd->initializer, ctx);
    break;
  }
  case AST_NEW_EXPRESSION: {
    NewExpression *ne = (NewExpression *)node->data;
    if (ne && ne->type_name) {
      char *base = NULL;
      char **args = NULL;
      size_t arg_count = 0;
      if (parse_generic_type_name(ne->type_name, &base, &args, &arg_count)) {
        char *mangled = mangle_name(base, args, arg_count);
        mettle_free_string(ne->type_name);
        ne->type_name = mangled;
        free(base);
        for (size_t i = 0; i < arg_count; i++)
          free(args[i]);
        free(args);
      }
    }
    break;
  }
  case AST_FUNCTION_CALL: {
    CallExpression *ce = (CallExpression *)node->data;
    if (ce && ce->type_arg_count > 0 && ce->function_name) {
      char *mangled =
          mangle_name(ce->function_name, ce->type_args, ce->type_arg_count);
      mettle_free_string(ce->function_name);
      ce->function_name = mangled;
      for (size_t i = 0; i < ce->type_arg_count; i++)
        mettle_free_string(ce->type_args[i]);
      free(ce->type_args);
      ce->type_args = NULL;
      ce->type_arg_count = 0;
    }
    if (ce) {
      for (size_t i = 0; i < ce->argument_count; i++)
        rewrite_generic_references(ce->arguments[i], ctx);
      if (ce->object)
        rewrite_generic_references(ce->object, ctx);
    }
    break;
  }
  case AST_FUNCTION_DECLARATION: {
    FunctionDeclaration *fd = (FunctionDeclaration *)node->data;
    if (fd && fd->type_param_count == 0) {
      for (size_t i = 0; i < fd->parameter_count; i++) {
        if (fd->parameter_types[i]) {
          rewrite_generic_type_name_in_place(&fd->parameter_types[i], NULL,
                                             NULL, 0);
        }
      }
      if (fd->return_type) {
        rewrite_generic_type_name_in_place(&fd->return_type, NULL, NULL, 0);
      }
      if (fd->body)
        rewrite_generic_references(fd->body, ctx);
    }
    break;
  }
  case AST_STRUCT_DECLARATION: {
    StructDeclaration *sd = (StructDeclaration *)node->data;
    if (sd && sd->type_param_count == 0) {
      for (size_t i = 0; i < sd->field_count; i++) {
        if (sd->field_types[i]) {
          rewrite_generic_type_name_in_place(&sd->field_types[i], NULL, NULL,
                                             0);
        }
      }
    }
    break;
  }
  case AST_PROGRAM: {
    Program *prog = (Program *)node->data;
    if (prog) {
      for (size_t i = 0; i < prog->declaration_count; i++)
        rewrite_generic_references(prog->declarations[i], ctx);
    }
    break;
  }
  case AST_RETURN_STATEMENT: {
    ReturnStatement *rs = (ReturnStatement *)node->data;
    if (rs && rs->value)
      rewrite_generic_references(rs->value, ctx);
    break;
  }
  case AST_ASSIGNMENT: {
    Assignment *as = (Assignment *)node->data;
    if (as) {
      if (as->value)
        rewrite_generic_references(as->value, ctx);
      if (as->target)
        rewrite_generic_references(as->target, ctx);
    }
    break;
  }
  case AST_IF_STATEMENT: {
    IfStatement *is_stmt = (IfStatement *)node->data;
    if (is_stmt) {
      rewrite_generic_references(is_stmt->condition, ctx);
      rewrite_generic_references(is_stmt->then_branch, ctx);
      for (size_t i = 0; i < is_stmt->else_if_count; i++) {
        rewrite_generic_references(is_stmt->else_ifs[i].condition, ctx);
        rewrite_generic_references(is_stmt->else_ifs[i].body, ctx);
      }
      if (is_stmt->else_branch)
        rewrite_generic_references(is_stmt->else_branch, ctx);
    }
    break;
  }
  case AST_WHILE_STATEMENT: {
    WhileStatement *ws = (WhileStatement *)node->data;
    if (ws) {
      rewrite_generic_references(ws->condition, ctx);
      rewrite_generic_references(ws->body, ctx);
    }
    break;
  }
  case AST_FOR_STATEMENT: {
    ForStatement *fs = (ForStatement *)node->data;
    if (fs) {
      if (fs->initializer)
        rewrite_generic_references(fs->initializer, ctx);
      if (fs->condition)
        rewrite_generic_references(fs->condition, ctx);
      if (fs->increment)
        rewrite_generic_references(fs->increment, ctx);
      if (fs->body)
        rewrite_generic_references(fs->body, ctx);
    }
    break;
  }
  case AST_SWITCH_STATEMENT: {
    SwitchStatement *ss = (SwitchStatement *)node->data;
    if (ss) {
      rewrite_generic_references(ss->expression, ctx);
      for (size_t i = 0; i < ss->case_count; i++)
        rewrite_generic_references(ss->cases[i], ctx);
    }
    break;
  }
  case AST_CASE_CLAUSE: {
    CaseClause *cc = (CaseClause *)node->data;
    if (cc) {
      if (cc->value)
        rewrite_generic_references(cc->value, ctx);
      if (cc->body)
        rewrite_generic_references(cc->body, ctx);
    }
    break;
  }
  case AST_BINARY_EXPRESSION: {
    BinaryExpression *be = (BinaryExpression *)node->data;
    if (be) {
      rewrite_generic_references(be->left, ctx);
      rewrite_generic_references(be->right, ctx);
    }
    break;
  }
  case AST_UNARY_EXPRESSION: {
    UnaryExpression *ue = (UnaryExpression *)node->data;
    if (ue && ue->operand)
      rewrite_generic_references(ue->operand, ctx);
    break;
  }
  case AST_MEMBER_ACCESS: {
    MemberAccess *ma = (MemberAccess *)node->data;
    if (ma && ma->object)
      rewrite_generic_references(ma->object, ctx);
    break;
  }
  case AST_INDEX_EXPRESSION: {
    ArrayIndexExpression *ie = (ArrayIndexExpression *)node->data;
    if (ie) {
      if (ie->array)
        rewrite_generic_references(ie->array, ctx);
      if (ie->index)
        rewrite_generic_references(ie->index, ctx);
    }
    break;
  }
  case AST_DEFER_STATEMENT:
  case AST_ERRDEFER_STATEMENT: {
    DeferStatement *ds = (DeferStatement *)node->data;
    if (ds && ds->statement)
      rewrite_generic_references(ds->statement, ctx);
    break;
  }
  default:
    break;
  }
}

static GenericDef *find_generic_def(MonoContext *ctx, const char *name) {
  for (size_t i = 0; i < ctx->def_count; i++) {
    if (strcmp(ctx->defs[i].name, name) == 0)
      return &ctx->defs[i];
  }
  return NULL;
}

static int validate_instantiation(GenericDef *def, Instantiation *inst,
                                  MonoContext *ctx) {
  if (!def || !inst || !ctx) {
    return 0;
  }

  if (def->type_param_count != inst->type_arg_count) {
    char message[512];
    snprintf(message, sizeof(message),
             "Generic '%s' expects %zu type arguments but got %zu", def->name,
             def->type_param_count, inst->type_arg_count);
    mono_report_error(ctx, inst->location, message);
    return 0;
  }

  for (size_t i = 0; i < def->type_param_count; i++) {
    const char *bounds =
        def->type_param_traits ? def->type_param_traits[i] : NULL;
    if (!mono_type_satisfies_bounds(ctx, def, inst, bounds, i)) {
      return 0;
    }
  }

  return 1;
}

static ASTNode *create_monomorphized_struct(GenericDef *def,
                                            Instantiation *inst,
                                            MonoContext *ctx) {
  ASTNode *clone = ast_clone_node(def->node);
  if (!clone)
    return NULL;

  StructDeclaration *sd = (StructDeclaration *)clone->data;

  // Set the mangled name
  mettle_free_string(sd->name);
  sd->name = strdup(inst->mangled_name);

  // Clear type params (this is now a concrete type)
  for (size_t i = 0; i < sd->type_param_count; i++)
    mettle_free_string(sd->type_params[i]);
  free(sd->type_params);
  for (size_t i = 0; i < sd->type_param_count; i++)
    mettle_free_string(sd->type_param_traits[i]);
  free(sd->type_param_traits);
  sd->type_params = NULL;
  sd->type_param_traits = NULL;
  sd->type_param_count = 0;

  // Substitute type params in field types
  for (size_t i = 0; i < sd->field_count; i++) {
    char *new_type = substitute_type_string(
        sd->field_types[i], def->type_params, inst->type_args,
        inst->type_arg_count, ctx);
    if (new_type) {
      mettle_free_string(sd->field_types[i]);
      sd->field_types[i] = new_type;
    }
  }

  return clone;
}

static ASTNode *create_monomorphized_function(GenericDef *def,
                                              Instantiation *inst,
                                              MonoContext *ctx) {
  ASTNode *clone = ast_clone_node(def->node);
  if (!clone)
    return NULL;

  FunctionDeclaration *fd = (FunctionDeclaration *)clone->data;

  // Set the mangled name
  mettle_free_string(fd->name);
  fd->name = strdup(inst->mangled_name);

  // Clear type params
  for (size_t i = 0; i < fd->type_param_count; i++)
    mettle_free_string(fd->type_params[i]);
  free(fd->type_params);
  for (size_t i = 0; i < fd->type_param_count; i++)
    mettle_free_string(fd->type_param_traits[i]);
  free(fd->type_param_traits);
  fd->type_params = NULL;
  fd->type_param_traits = NULL;
  fd->type_param_count = 0;

  // Substitute type params in parameter types
  for (size_t i = 0; i < fd->parameter_count; i++) {
    char *new_type = substitute_type_string(
        fd->parameter_types[i], def->type_params, inst->type_args,
        inst->type_arg_count, ctx);
    if (new_type) {
      mettle_free_string(fd->parameter_types[i]);
      fd->parameter_types[i] = new_type;
    }
  }

  // Substitute type params in return type
  if (fd->return_type) {
    char *new_type = substitute_type_string(
        fd->return_type, def->type_params, inst->type_args,
        inst->type_arg_count, ctx);
    if (new_type) {
      mettle_free_string(fd->return_type);
      fd->return_type = new_type;
    }
  }

  // Substitute type params throughout the function body
  if (fd->body) {
    substitute_types_in_ast(fd->body, def->type_params, inst->type_args,
                            inst->type_arg_count, ctx);
  }

  return clone;
}

typedef struct {
  char *name;
  char *type_name;
} MonoVarBinding;

typedef struct {
  MonoVarBinding *items;
  size_t count;
  size_t capacity;
} MonoVarEnv;

static int mono_env_add(MonoVarEnv *env, char *name, char *type_name) {
  if (!env || !name || !type_name) {
    return 1;
  }

  for (size_t i = 0; i < env->count; i++) {
    if (env->items[i].name && strcmp(env->items[i].name, name) == 0) {
      env->items[i].type_name = type_name;
      return 1;
    }
  }

  if (env->count >= env->capacity) {
    size_t new_capacity = env->capacity ? env->capacity * 2 : 8;
    MonoVarBinding *grown =
        realloc(env->items, new_capacity * sizeof(MonoVarBinding));
    if (!grown) {
      return 0;
    }
    env->items = grown;
    env->capacity = new_capacity;
  }

  env->items[env->count].name = name;
  env->items[env->count].type_name = type_name;
  env->count++;
  return 1;
}

static const char *mono_env_lookup(MonoVarEnv *env, const char *name) {
  if (!env || !name) {
    return NULL;
  }

  for (size_t i = env->count; i > 0; i--) {
    if (env->items[i - 1].name && strcmp(env->items[i - 1].name, name) == 0) {
      return env->items[i - 1].type_name;
    }
  }

  return NULL;
}

static TraitImpl *mono_find_impl_for_method(MonoContext *ctx,
                                            const char *type_name,
                                            const char *method_name,
                                            int *ambiguous) {
  TraitImpl *found = NULL;

  if (ambiguous) {
    *ambiguous = 0;
  }
  if (!ctx || !type_name || !method_name) {
    return NULL;
  }

  for (size_t i = 0; i < ctx->impl_count; i++) {
    TraitImpl *impl = &ctx->impls[i];
    if (!impl->for_type_name || strcmp(impl->for_type_name, type_name) != 0) {
      continue;
    }
    if (!mono_find_function_method(impl->methods, impl->method_count,
                                   method_name)) {
      continue;
    }
    if (found) {
      if (ambiguous) {
        *ambiguous = 1;
      }
      return NULL;
    }
    found = impl;
  }

  return found;
}

static int mono_rewrite_trait_method_call(MonoContext *ctx, ASTNode *node,
                                          MonoVarEnv *env) {
  CallExpression *call = NULL;
  Identifier *object_id = NULL;
  const char *object_type = NULL;
  TraitImpl *impl = NULL;
  char *mangled = NULL;
  ASTNode **new_args = NULL;
  ASTNode *self_arg = NULL;
  int ambiguous = 0;

  if (!ctx || !node || node->type != AST_FUNCTION_CALL || !env) {
    return 1;
  }

  call = (CallExpression *)node->data;
  if (!call || !call->object || call->object->type != AST_IDENTIFIER ||
      !call->function_name) {
    return 1;
  }

  object_id = (Identifier *)call->object->data;
  if (!object_id || !object_id->name) {
    return 1;
  }

  object_type = mono_env_lookup(env, object_id->name);
  if (!object_type) {
    return 1;
  }

  impl = mono_find_impl_for_method(ctx, object_type, call->function_name,
                                   &ambiguous);
  if (ambiguous) {
    char message[512];
    snprintf(message, sizeof(message),
             "Trait method call '%s.%s' is ambiguous for type '%s'",
             object_id->name, call->function_name, object_type);
    mono_report_error(ctx, node->location, message);
    return 0;
  }
  if (!impl) {
    return 1;
  }

  mangled = mono_mangle_impl_method_name(impl->trait_name, impl->for_type_name,
                                         call->function_name);
  self_arg = ast_clone_node(call->object);
  new_args = malloc((call->argument_count + 1) * sizeof(ASTNode *));
  if (!mangled || !self_arg || !new_args) {
    free(mangled);
    if (self_arg) {
      ast_destroy_node(self_arg);
    }
    free(new_args);
    mono_report_error(ctx, node->location,
                      "Failed to rewrite trait method call");
    return 0;
  }

  new_args[0] = self_arg;
  for (size_t i = 0; i < call->argument_count; i++) {
    new_args[i + 1] = call->arguments[i];
  }
  free(call->arguments);
  call->arguments = new_args;
  call->argument_count++;
  ast_add_child(node, self_arg);

  mettle_free_string(call->function_name);
  call->function_name = mangled;
  call->object = NULL;
  return 1;
}

static int mono_rewrite_trait_method_calls_in_node(MonoContext *ctx,
                                                   ASTNode *node,
                                                   MonoVarEnv *env) {
  if (!node || !env) {
    return 1;
  }

  if (node->type == AST_FUNCTION_CALL) {
    CallExpression *call = (CallExpression *)node->data;
    if (call) {
      for (size_t i = 0; i < call->argument_count; i++) {
        if (!mono_rewrite_trait_method_calls_in_node(ctx, call->arguments[i],
                                                     env)) {
          return 0;
        }
      }
      if (call->object &&
          !mono_rewrite_trait_method_calls_in_node(ctx, call->object, env)) {
        return 0;
      }
    }
    return mono_rewrite_trait_method_call(ctx, node, env);
  }

  if (node->type == AST_VAR_DECLARATION) {
    VarDeclaration *decl = (VarDeclaration *)node->data;
    if (decl && decl->initializer &&
        !mono_rewrite_trait_method_calls_in_node(ctx, decl->initializer, env)) {
      return 0;
    }
    if (decl && !mono_env_add(env, decl->name, decl->type_name)) {
      mono_report_error(ctx, node->location,
                        "Failed to track local type for trait method call");
      return 0;
    }
    return 1;
  }

  for (size_t i = 0; i < node->child_count; i++) {
    if (!mono_rewrite_trait_method_calls_in_node(ctx, node->children[i], env)) {
      return 0;
    }
  }

  return 1;
}

static int mono_rewrite_trait_method_calls_in_function(MonoContext *ctx,
                                                       ASTNode *function) {
  FunctionDeclaration *fn = NULL;
  MonoVarEnv env = {0};
  int ok = 1;

  if (!ctx || !function || function->type != AST_FUNCTION_DECLARATION) {
    return 1;
  }

  fn = (FunctionDeclaration *)function->data;
  if (!fn || !fn->body) {
    return 1;
  }

  for (size_t i = 0; i < fn->parameter_count; i++) {
    if (!mono_env_add(&env, fn->parameter_names[i], fn->parameter_types[i])) {
      mono_report_error(ctx, function->location,
                        "Failed to track parameter type for trait method call");
      ok = 0;
      goto cleanup;
    }
  }

  ok = mono_rewrite_trait_method_calls_in_node(ctx, fn->body, &env);

cleanup:
  free(env.items);
  return ok;
}

static int mono_rewrite_trait_method_calls(MonoContext *ctx, ASTNode *program) {
  Program *prog = NULL;

  if (!ctx || !program || program->type != AST_PROGRAM) {
    return 1;
  }

  prog = (Program *)program->data;
  if (!prog) {
    return 1;
  }

  for (size_t i = 0; i < prog->declaration_count; i++) {
    ASTNode *decl = prog->declarations[i];
    if (decl && decl->type == AST_FUNCTION_DECLARATION) {
      FunctionDeclaration *fn = (FunctionDeclaration *)decl->data;
      if (fn && fn->type_param_count == 0 &&
          !mono_rewrite_trait_method_calls_in_function(ctx, decl)) {
        return 0;
      }
    }
  }

  return 1;
}

static int mono_emit_impl_method_functions(MonoContext *ctx, ASTNode *program) {
  Program *prog = NULL;

  if (!ctx || !program || program->type != AST_PROGRAM) {
    return 1;
  }

  prog = (Program *)program->data;
  if (!prog) {
    return 1;
  }

  for (size_t i = 0; i < ctx->impl_count; i++) {
    TraitImpl *impl = &ctx->impls[i];
    for (size_t j = 0; j < impl->method_count; j++) {
      ASTNode *fn = mono_create_impl_method_function(ctx, impl, impl->methods[j]);
      ASTNode **grown = NULL;
      if (!fn) {
        mono_report_error(ctx, (SourceLocation){0, 0, NULL},
                          "Failed to create trait impl method function");
        return 0;
      }
      grown = realloc(prog->declarations,
                      (prog->declaration_count + 1) * sizeof(ASTNode *));
      if (!grown) {
        SourceLocation location = fn->location;
        ast_destroy_node(fn);
        mono_report_error(ctx, location,
                          "Failed to append trait impl method function");
        return 0;
      }
      prog->declarations = grown;
      prog->declarations[prog->declaration_count++] = fn;
      ast_add_child(program, fn);
    }
  }

  return 1;
}

static int mono_emit_instantiation(MonoContext *ctx, Program *prog,
                                   ASTNode *program, size_t index,
                                   int *success) {
  ASTNode *mono_node = NULL;
  GenericDef *def = NULL;
  Instantiation *inst = NULL;
  size_t deps_start = 0;

  if (!ctx || !prog || !program || !success || index >= ctx->instance_count) {
    return 1;
  }

  inst = &ctx->instances[index];
  if (inst->emitted) {
    return 1;
  }

  def = find_generic_def(ctx, inst->generic_name);
  if (!def) {
    return 1;
  }

  if (!validate_instantiation(def, inst, ctx)) {
    *success = 0;
    return 0;
  }

  deps_start = ctx->instance_count;

  if (def->is_struct) {
    mono_node = create_monomorphized_struct(def, inst, ctx);
  } else {
    mono_node = create_monomorphized_function(def, inst, ctx);
  }

  if (!mono_node) {
    return 1;
  }

  for (size_t j = deps_start; j < ctx->instance_count; j++) {
    GenericDef *dep_def = find_generic_def(ctx, ctx->instances[j].generic_name);
    if (!dep_def || !dep_def->is_struct || ctx->instances[j].emitted) {
      continue;
    }
    if (!mono_emit_instantiation(ctx, prog, program, j, success)) {
      return 0;
    }
    if (!*success) {
      return 0;
    }
  }

  prog->declarations =
      realloc(prog->declarations,
              (prog->declaration_count + 1) * sizeof(ASTNode *));
  if (!prog->declarations) {
    *success = 0;
    return 0;
  }
  prog->declarations[prog->declaration_count] = mono_node;
  prog->declaration_count++;
  ast_add_child(program, mono_node);

  inst->emitted = 1;
  collect_type_instantiations(mono_node, ctx);
  return 1;
}

int monomorphize_program(ASTNode *program, ErrorReporter *reporter) {
  if (!program || program->type != AST_PROGRAM)
    return 1;

  Program *prog = (Program *)program->data;
  if (!prog)
    return 1;

  MonoContext ctx = {0};
  int success = 1;
  ctx.reporter = reporter;

  // Step 1: Collect generic definitions
  collect_generic_defs(program, &ctx);
  if (ctx.had_error) {
    success = 0;
    goto cleanup;
  }

  if (!mono_emit_impl_method_functions(&ctx, program)) {
    success = 0;
    goto cleanup;
  }

  if (ctx.def_count > 0) {
    // Step 2: Collect all instantiations from non-generic code
    collect_type_instantiations(program, &ctx);

    // Step 3: Generate monomorphized definitions, iterating until no new
    // instantiations are discovered (handles transitive generic usage).
    size_t processed = 0;
    while (processed < ctx.instance_count) {
      size_t current_count = ctx.instance_count;
      for (size_t i = processed; i < current_count; i++) {
        if (!mono_emit_instantiation(&ctx, prog, program, i, &success)) {
          goto cleanup;
        }
        if (!success) {
          goto cleanup;
        }
      }
      processed = current_count;
    }

    // Step 4: Rewrite all generic references to use mangled names
    rewrite_generic_references(program, &ctx);
  }

  if (!mono_rewrite_trait_method_calls(&ctx, program)) {
    success = 0;
    goto cleanup;
  }

  // Step 5: Remove generic (template) definitions from the program
  size_t write_idx = 0;
  for (size_t i = 0; i < prog->declaration_count; i++) {
    ASTNode *decl = prog->declarations[i];
    int is_generic = 0;
    int is_compile_time_trait_decl = 0;

    if (decl && decl->type == AST_STRUCT_DECLARATION) {
      StructDeclaration *sd = (StructDeclaration *)decl->data;
      if (sd && sd->type_param_count > 0)
        is_generic = 1;
    } else if (decl && decl->type == AST_FUNCTION_DECLARATION) {
      FunctionDeclaration *fd = (FunctionDeclaration *)decl->data;
      if (fd && fd->type_param_count > 0)
        is_generic = 1;
    } else if (decl &&
               (decl->type == AST_TRAIT_DECLARATION ||
                decl->type == AST_IMPL_DECLARATION)) {
      is_compile_time_trait_decl = 1;
    }

    if (!is_generic && !is_compile_time_trait_decl) {
      prog->declarations[write_idx++] = decl;
    }
  }
  prog->declaration_count = write_idx;

cleanup:
  // Clean up context
  for (size_t i = 0; i < ctx.trait_count; i++) {
    free(ctx.traits[i].name);
  }
  free(ctx.traits);

  for (size_t i = 0; i < ctx.impl_count; i++) {
    free(ctx.impls[i].trait_name);
    free(ctx.impls[i].for_type_name);
  }
  free(ctx.impls);

  for (size_t i = 0; i < ctx.def_count; i++) {
    free(ctx.defs[i].name);
    for (size_t j = 0; j < ctx.defs[i].type_param_count; j++)
      free(ctx.defs[i].type_params[j]);
    free(ctx.defs[i].type_params);
    for (size_t j = 0; j < ctx.defs[i].type_param_count; j++)
      free(ctx.defs[i].type_param_traits[j]);
    free(ctx.defs[i].type_param_traits);
  }
  free(ctx.defs);

  for (size_t i = 0; i < ctx.instance_count; i++) {
    free(ctx.instances[i].generic_name);
    free(ctx.instances[i].mangled_name);
    for (size_t j = 0; j < ctx.instances[i].type_arg_count; j++)
      free(ctx.instances[i].type_args[j]);
    free(ctx.instances[i].type_args);
  }
  free(ctx.instances);

  return success && !ctx.had_error;
}
