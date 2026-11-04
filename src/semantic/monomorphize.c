#include "monomorphize.h"
#include "common.h"
#include "string_intern.h"
#include "common.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
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
  int is_struct;
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
  Program *module;
  ASTNode **decl_slots;
  size_t decl_slot_count;
  size_t decls_indexed;
} MonoContext;

static char *substitute_type_string(const char *type_str, char **param_names,
                                    char **arg_names, size_t count,
                                    MonoContext *ctx);
static void substitute_types_in_ast(ASTNode *node, char **param_names,
                                    char **arg_names, size_t count,
                                    MonoContext *ctx);
static GenericDef *find_generic_def(MonoContext *ctx, const char *name);

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

static char *written_generic_name(const char *base, char **type_args,
                                  size_t type_arg_count) {
  size_t len = strlen(base) + 3;
  for (size_t i = 0; i < type_arg_count; i++) {
    len += strlen(type_args[i]) + 2;
  }

  char *result = malloc(len);
  if (!result)
    return NULL;

  size_t pos = (size_t)snprintf(result, len, "%s<", base);
  for (size_t i = 0; i < type_arg_count; i++) {
    pos += (size_t)snprintf(result + pos, len - pos, "%s%s", i ? ", " : "",
                            type_args[i]);
  }
  snprintf(result + pos, len - pos, ">");

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

  const char *suffix = gt + 1;
  if (suffix < end && *suffix != '\0') {
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

  for (size_t i = 0; i < arg_count; i++) {
    record_generic_type_use(ctx, args[i], location);
  }

  mono_add_instantiation(ctx, base, args, arg_count, location);
  free(base);
  free_string_array(args, arg_count);
}

static char *substitute_type_string(const char *type_str, char **param_names,
                                    char **arg_names, size_t count,
                                    MonoContext *ctx);

static char *substitute_function_type_string(const char *type_str,
                                             char **param_names,
                                             char **arg_names, size_t count,
                                             MonoContext *ctx) {
  size_t head = 3;
  const char *params = type_str + head;
  const char *scan = params;
  int depth = 1;
  while (*scan && depth > 0) {
    if (*scan == '(')
      depth++;
    else if (*scan == ')')
      depth--;
    scan++;
  }
  if (depth != 0 || scan[0] != '-' || scan[1] != '>')
    return NULL;
  const char *params_end = scan - 1;
  const char *ret = scan + 2;
  const char *ret_end = ret;
  int angle = 0;
  int paren = 0;
  while (*ret_end) {
    if (*ret_end == '<')
      angle++;
    else if (*ret_end == '>')
      angle--;
    else if (*ret_end == '(')
      paren++;
    else if (*ret_end == ')')
      paren--;
    else if (*ret_end == ' ' && angle == 0 && paren == 0)
      break;
    ret_end++;
  }

  size_t cap = strlen(type_str) * 4 + 64;
  for (size_t i = 0; i < count; i++)
    cap += strlen(arg_names[i]) * 8;
  char *out = malloc(cap);
  if (!out)
    return NULL;
  size_t len = 0;
  memcpy(out, type_str, head);
  len = head;

  const char *item = params;
  int item_angle = 0;
  int item_paren = 0;
  for (const char *c = params; c <= params_end; c++) {
    int at_end = c == params_end;
    if (!at_end) {
      if (*c == '<')
        item_angle++;
      else if (*c == '>')
        item_angle--;
      else if (*c == '(')
        item_paren++;
      else if (*c == ')')
        item_paren--;
    }
    if (at_end || (*c == ',' && item_angle == 0 && item_paren == 0)) {
      if (c > item) {
        char *piece = malloc((size_t)(c - item) + 1);
        memcpy(piece, item, (size_t)(c - item));
        piece[c - item] = '\0';
        char *swapped =
            substitute_type_string(piece, param_names, arg_names, count, ctx);
        free(piece);
        size_t need = len + strlen(swapped) + strlen(ret_end) + 16;
        if (need > cap) {
          cap = need * 2;
          out = realloc(out, cap);
        }
        memcpy(out + len, swapped, strlen(swapped));
        len += strlen(swapped);
        free(swapped);
      }
      if (!at_end)
        out[len++] = ',';
      item = c + 1;
    }
  }
  out[len++] = ')';
  out[len++] = '-';
  out[len++] = '>';

  char *ret_piece = malloc((size_t)(ret_end - ret) + 1);
  memcpy(ret_piece, ret, (size_t)(ret_end - ret));
  ret_piece[ret_end - ret] = '\0';
  char *ret_swapped =
      substitute_type_string(ret_piece, param_names, arg_names, count, ctx);
  free(ret_piece);
  size_t need = len + strlen(ret_swapped) + strlen(ret_end) + 1;
  if (need > cap) {
    cap = need;
    out = realloc(out, cap);
  }
  memcpy(out + len, ret_swapped, strlen(ret_swapped));
  len += strlen(ret_swapped);
  free(ret_swapped);
  strcpy(out + len, ret_end);
  return out;
}

static char *substitute_type_string(const char *type_str, char **param_names,
                                    char **arg_names, size_t count,
                                    MonoContext *ctx) {
  if (!type_str)
    return NULL;

  if ((strncmp(type_str, "fn(", 3) == 0 || strncmp(type_str, "Fn(", 3) == 0)) {
    char *function_type = substitute_function_type_string(
        type_str, param_names, arg_names, count, ctx);
    if (function_type)
      return function_type;
  }

  size_t ts_len = strlen(type_str);

  size_t core_len = ts_len;
  size_t ptr_count = 0;
  char *array_suffix = NULL;

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

  while (core_len > 0 && type_str[core_len - 1] == '*') {
    ptr_count++;
    core_len--;
  }

  char *core = malloc(core_len + 1);
  memcpy(core, type_str, core_len);
  core[core_len] = '\0';

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

  char *gen_base = NULL;
  char **gen_args = NULL;
  size_t gen_arg_count = 0;
  if (parse_generic_type_name(core, &gen_base, &gen_args, &gen_arg_count)) {
    char **subst_args = malloc(gen_arg_count * sizeof(char *));
    for (size_t i = 0; i < gen_arg_count; i++) {
      subst_args[i] =
          substitute_type_string(gen_args[i], param_names, arg_names, count, ctx);
    }

    int owned = !ctx || find_generic_def(ctx, gen_base) != NULL;
    char *mangled = owned ? mangle_name(gen_base, subst_args, gen_arg_count)
                          : written_generic_name(gen_base, subst_args,
                                                 gen_arg_count);

    if (ctx && owned) {
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
      if (ce->function_name && strcmp(ce->function_name, "sizeof") == 0 &&
          ce->argument_count == 1 && ce->arguments[0] &&
          ce->arguments[0]->type == AST_IDENTIFIER) {
        Identifier *type_id = (Identifier *)ce->arguments[0]->data;
        char *new_name =
            type_id && type_id->name
                ? substitute_type_string(type_id->name, param_names, arg_names,
                                         count, ctx)
                : NULL;
        if (new_name) {
          mettle_free_string(type_id->name);
          type_id->name = new_name;
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
  case AST_MATCH_STATEMENT: {
    MatchStatement *ms = (MatchStatement *)node->data;
    if (ms) {
      substitute_types_in_ast(ms->expression, param_names, arg_names, count, ctx);
      for (size_t i = 0; i < ms->arm_count; i++)
        substitute_types_in_ast(ms->arms[i].body, param_names, arg_names, count, ctx);
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
  case AST_CAST_EXPRESSION: {
    CastExpression *ce = (CastExpression *)node->data;
    if (ce) {
      if (ce->type_name) {
        char *new_type = substitute_type_string(ce->type_name, param_names,
                                                arg_names, count, ctx);
        if (new_type) {
          mettle_free_string(ce->type_name);
          ce->type_name = new_type;
        }
      }
      substitute_types_in_ast(ce->operand, param_names, arg_names, count, ctx);
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
  case AST_GPU_LAUNCH:
    for (size_t i = 0; i < node->child_count; i++) {
      substitute_types_in_ast(node->children[i], param_names, arg_names, count,
                              ctx);
    }
    break;
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
  case AST_STRUCT_DECLARATION: {
    StructDeclaration *sd = (StructDeclaration *)node->data;
    if (sd && sd->type_param_count == 0) {
      for (size_t i = 0; i < sd->field_count; i++) {
        record_generic_type_use(ctx, sd->field_types[i], node->location);
      }
      for (size_t i = 0; i < sd->method_count; i++) {
        ASTNode *method = sd->methods[i];
        FunctionDeclaration *md =
            method ? (FunctionDeclaration *)method->data : NULL;
        if (!md) {
          continue;
        }
        for (size_t p = 0; p < md->parameter_count; p++) {
          record_generic_type_use(ctx, md->parameter_types[p],
                                  method->location);
        }
        record_generic_type_use(ctx, md->return_type, method->location);
        if (md->body) {
          collect_type_instantiations(md->body, ctx);
        }
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
  case AST_MATCH_STATEMENT: {
    MatchStatement *ms = (MatchStatement *)node->data;
    if (ms) {
      collect_type_instantiations(ms->expression, ctx);
      for (size_t i = 0; i < ms->arm_count; i++)
        collect_type_instantiations(ms->arms[i].body, ctx);
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
  case AST_CAST_EXPRESSION: {
    CastExpression *ce = (CastExpression *)node->data;
    if (ce && ce->operand)
      collect_type_instantiations(ce->operand, ctx);
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
  case AST_GPU_LAUNCH:
    for (size_t i = 0; i < node->child_count; i++) {
      collect_type_instantiations(node->children[i], ctx);
    }
    break;
  default:
    break;
  }
}

static GenericDef *find_generic_def(MonoContext *ctx, const char *name);

static void rewrite_generic_type_name_in_place(char **slot,
                                               const char **type_params,
                                               const char **concrete_types,
                                               size_t type_param_count,
                                               MonoContext *ctx) {
  char *type_str = *slot;
  size_t len = strlen(type_str);
  size_t ptr_count = 0;
  char *base = NULL;
  char **args = NULL;
  size_t arg_count = 0;
  const char *array_suffix = NULL;
  int depth = 0;

  (void)type_params;
  (void)concrete_types;
  (void)type_param_count;

  for (size_t i = 0; i < len; i++) {
    if (type_str[i] == '<') {
      depth++;
    } else if (type_str[i] == '>') {
      depth--;
    } else if (type_str[i] == '[' && depth == 0) {
      array_suffix = type_str + i;
      len = i;
      break;
    }
  }

  while (len > 0 && type_str[len - 1] == '*') {
    ptr_count++;
    len--;
  }

  char *core = malloc(len + 1);
  memcpy(core, type_str, len);
  core[len] = '\0';

  if (parse_generic_type_name(core, &base, &args, &arg_count)) {
    if (ctx && !find_generic_def(ctx, base)) {
      free(base);
      for (size_t i = 0; i < arg_count; i++)
        free(args[i]);
      free(args);
      free(core);
      return;
    }
    char *mangled = mangle_name(base, args, arg_count);
    size_t new_len = strlen(mangled) + ptr_count +
                     (array_suffix ? strlen(array_suffix) : 0) + 1;
    char *new_type = malloc(new_len);
    strcpy(new_type, mangled);
    for (size_t i = 0; i < ptr_count; i++)
      strcat(new_type, "*");
    if (array_suffix)
      strcat(new_type, array_suffix);
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
      rewrite_generic_type_name_in_place(&vd->type_name, NULL, NULL, 0, ctx);
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
      char *written = written_generic_name(ce->function_name, ce->type_args,
                                           ce->type_arg_count);
      char *mangled =
          mangle_name(ce->function_name, ce->type_args, ce->type_arg_count);
      mettle_free_string(ce->function_name);
      ce->function_name = mangled;
      if (written) {
        mettle_free_string(ce->written_name);
        ce->written_name = written;
      }
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
                                             NULL, 0, ctx);
        }
      }
      if (fd->return_type) {
        rewrite_generic_type_name_in_place(&fd->return_type, NULL, NULL, 0, ctx);
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
                                             0, ctx);
        }
      }
      for (size_t i = 0; i < sd->method_count; i++) {
        ASTNode *method = sd->methods[i];
        FunctionDeclaration *md =
            method ? (FunctionDeclaration *)method->data : NULL;
        if (!md) {
          continue;
        }
        for (size_t p = 0; p < md->parameter_count; p++) {
          if (md->parameter_types[p]) {
            rewrite_generic_type_name_in_place(&md->parameter_types[p], NULL,
                                               NULL, 0, ctx);
          }
        }
        if (md->return_type) {
          rewrite_generic_type_name_in_place(&md->return_type, NULL, NULL, 0, ctx);
        }
        if (md->body) {
          rewrite_generic_references(md->body, ctx);
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
  case AST_MATCH_STATEMENT: {
    MatchStatement *ms = (MatchStatement *)node->data;
    if (ms) {
      rewrite_generic_references(ms->expression, ctx);
      for (size_t i = 0; i < ms->arm_count; i++)
        rewrite_generic_references(ms->arms[i].body, ctx);
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
  case AST_CAST_EXPRESSION: {
    CastExpression *ce = (CastExpression *)node->data;
    if (ce && ce->operand)
      rewrite_generic_references(ce->operand, ctx);
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
  case AST_GPU_LAUNCH:
    for (size_t i = 0; i < node->child_count; i++) {
      rewrite_generic_references(node->children[i], ctx);
    }
    break;
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

  char **type_args = inst->type_args;
  size_t type_arg_count = inst->type_arg_count;

  mettle_free_string(sd->name);
  sd->name = strdup(inst->mangled_name);

  for (size_t i = 0; i < sd->type_param_count; i++)
    mettle_free_string(sd->type_params[i]);
  free(sd->type_params);
  for (size_t i = 0; i < sd->type_param_count; i++)
    mettle_free_string(sd->type_param_traits[i]);
  free(sd->type_param_traits);
  sd->type_params = NULL;
  sd->type_param_traits = NULL;
  sd->type_param_count = 0;

  for (size_t i = 0; i < sd->field_count; i++) {
    char *new_type = substitute_type_string(
        sd->field_types[i], def->type_params, type_args,
        type_arg_count, ctx);
    if (new_type) {
      mettle_free_string(sd->field_types[i]);
      sd->field_types[i] = new_type;
    }
  }

  for (size_t i = 0; i < sd->method_count; i++) {
    ASTNode *method = sd->methods[i];
    FunctionDeclaration *md =
        method ? (FunctionDeclaration *)method->data : NULL;
    if (!md) {
      continue;
    }

    for (size_t p = 0; p < md->parameter_count; p++) {
      char *new_type = substitute_type_string(
          md->parameter_types[p], def->type_params, type_args,
          type_arg_count, ctx);
      if (new_type) {
        mettle_free_string(md->parameter_types[p]);
        md->parameter_types[p] = new_type;
      }
    }

    if (md->return_type) {
      char *new_type = substitute_type_string(
          md->return_type, def->type_params, type_args,
          type_arg_count, ctx);
      if (new_type) {
        mettle_free_string(md->return_type);
        md->return_type = new_type;
      }
    }

    if (md->body) {
      substitute_types_in_ast(md->body, def->type_params, type_args,
                              type_arg_count, ctx);
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

  char **type_args = inst->type_args;
  size_t type_arg_count = inst->type_arg_count;

  mettle_free_string(fd->name);
  fd->name = strdup(inst->mangled_name);

  for (size_t i = 0; i < fd->type_param_count; i++)
    mettle_free_string(fd->type_params[i]);
  free(fd->type_params);
  for (size_t i = 0; i < fd->type_param_count; i++)
    mettle_free_string(fd->type_param_traits[i]);
  free(fd->type_param_traits);
  fd->type_params = NULL;
  fd->type_param_traits = NULL;
  fd->type_param_count = 0;

  for (size_t i = 0; i < fd->parameter_count; i++) {
    char *new_type = substitute_type_string(
        fd->parameter_types[i], def->type_params, type_args,
        type_arg_count, ctx);
    if (new_type) {
      mettle_free_string(fd->parameter_types[i]);
      fd->parameter_types[i] = new_type;
    }
  }

  if (fd->return_type) {
    char *new_type = substitute_type_string(
        fd->return_type, def->type_params, type_args,
        type_arg_count, ctx);
    if (new_type) {
      mettle_free_string(fd->return_type);
      fd->return_type = new_type;
    }
  }

  if (fd->body) {
    substitute_types_in_ast(fd->body, def->type_params, type_args,
                            type_arg_count, ctx);
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
  if (!env || !name) {
    return 1;
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

static const MonoVarBinding *mono_env_find(const MonoVarEnv *env,
                                           const char *name) {
  if (!env || !name) {
    return NULL;
  }

  for (size_t i = env->count; i > 0; i--) {
    if (env->items[i - 1].name && strcmp(env->items[i - 1].name, name) == 0) {
      return &env->items[i - 1];
    }
  }

  return NULL;
}

static const char *mono_env_lookup(MonoVarEnv *env, const char *name) {
  const MonoVarBinding *binding = mono_env_find(env, name);
  return binding ? binding->type_name : NULL;
}

static int mono_node_opens_scope(const ASTNode *node) {
  return node->type == AST_PROGRAM || node->type == AST_FOR_STATEMENT ||
         node->type == AST_COMPTIME_FOR || node->type == AST_LAMBDA_EXPRESSION;
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

  if (!call->written_name) {
    call->written_name = strdup(call->function_name);
  }
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

  {
    size_t scope = env->count;
    for (size_t i = 0; i < node->child_count; i++) {
      if (!mono_rewrite_trait_method_calls_in_node(ctx, node->children[i],
                                                   env)) {
        return 0;
      }
    }
    if (mono_node_opens_scope(node)) {
      env->count = scope;
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

typedef struct {
  const char *name;
  char *bound;
} MonoTypeBinding;

static char *mono_text_trim_dup(const char *text) {
  size_t start = 0;
  size_t end;
  char *copy;

  if (!text) {
    return NULL;
  }
  end = strlen(text);
  while (start < end && (text[start] == ' ' || text[start] == '\t')) {
    start++;
  }
  while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t')) {
    end--;
  }
  copy = malloc(end - start + 1);
  if (!copy) {
    return NULL;
  }
  memcpy(copy, text + start, end - start);
  copy[end - start] = '\0';
  return copy;
}

static int mono_type_param_index(char **params, size_t count,
                                 const char *text) {
  size_t i;
  if (!params || !text) {
    return -1;
  }
  for (i = 0; i < count; i++) {
    if (params[i] && strcmp(params[i], text) == 0) {
      return (int)i;
    }
  }
  return -1;
}

static const char *mono_generic_args_start(const char *text) {
  const char *open = text ? strchr(text, '<') : NULL;
  if (!open || open == text) {
    return NULL;
  }
  if (text[strlen(text) - 1] != '>') {
    return NULL;
  }
  return open;
}

static int mono_match_type_text(const char *pattern, const char *actual,
                                char **params, size_t param_count,
                                MonoTypeBinding *bindings);

static int mono_match_generic_args(const char *pattern, const char *actual,
                                   char **params, size_t param_count,
                                   MonoTypeBinding *bindings) {
  const char *p_open = mono_generic_args_start(pattern);
  const char *a_open = mono_generic_args_start(actual);
  const char *p_scan;
  const char *a_scan;
  size_t p_head;
  size_t a_head;

  if (!p_open || !a_open) {
    return 1;
  }
  p_head = (size_t)(p_open - pattern);
  a_head = (size_t)(a_open - actual);
  if (p_head != a_head || strncmp(pattern, actual, p_head) != 0) {
    return 1;
  }

  p_scan = p_open + 1;
  a_scan = a_open + 1;
  while (*p_scan && *a_scan) {
    const char *p_end = p_scan;
    const char *a_end = a_scan;
    int depth = 0;
    char *p_arg;
    char *a_arg;
    int ok;
    while (*p_end && (depth > 0 || (*p_end != ',' && *p_end != '>'))) {
      if (*p_end == '<') {
        depth++;
      } else if (*p_end == '>') {
        depth--;
      }
      p_end++;
    }
    depth = 0;
    while (*a_end && (depth > 0 || (*a_end != ',' && *a_end != '>'))) {
      if (*a_end == '<') {
        depth++;
      } else if (*a_end == '>') {
        depth--;
      }
      a_end++;
    }
    p_arg = mono_text_trim_dup(p_scan);
    a_arg = mono_text_trim_dup(a_scan);
    if (p_arg && a_arg) {
      p_arg[p_end - p_scan] = '\0';
      a_arg[a_end - a_scan] = '\0';
    }
    ok = p_arg && a_arg
             ? mono_match_type_text(p_arg, a_arg, params, param_count, bindings)
             : 1;
    free(p_arg);
    free(a_arg);
    if (!ok) {
      return 0;
    }
    if (*p_end != ',' || *a_end != ',') {
      break;
    }
    p_scan = p_end + 1;
    a_scan = a_end + 1;
  }
  return 1;
}

static int mono_match_type_text(const char *pattern, const char *actual,
                                char **params, size_t param_count,
                                MonoTypeBinding *bindings) {
  char *p = mono_text_trim_dup(pattern);
  char *a = mono_text_trim_dup(actual);
  int result = 1;
  int index;

  if (!p || !a || !*p || !*a) {
    free(p);
    free(a);
    return 1;
  }

  index = mono_type_param_index(params, param_count, p);
  if (index >= 0) {
    if (bindings[index].bound) {
      result = strcmp(bindings[index].bound, a) == 0;
    } else {
      bindings[index].bound = mettle_strdup(a);
    }
    free(p);
    free(a);
    return result;
  }

  {
    size_t p_len = strlen(p);
    size_t a_len = strlen(a);
    if (p[p_len - 1] == '*' && a[a_len - 1] == '*') {
      p[p_len - 1] = '\0';
      a[a_len - 1] = '\0';
      result = mono_match_type_text(p, a, params, param_count, bindings);
      free(p);
      free(a);
      return result;
    }
    if (p[p_len - 1] == ']' && a[a_len - 1] == ']') {
      char *p_open = strrchr(p, '[');
      char *a_open = strrchr(a, '[');
      if (p_open && a_open) {
        *p_open = '\0';
        *a_open = '\0';
        result = mono_match_type_text(p, a, params, param_count, bindings);
        free(p);
        free(a);
        return result;
      }
    }
  }

  result = mono_match_generic_args(p, a, params, param_count, bindings);
  free(p);
  free(a);
  return result;
}

static const char *mono_decl_index_name(const ASTNode *decl) {
  if (!decl || !decl->data) {
    return NULL;
  }
  switch (decl->type) {
  case AST_FUNCTION_DECLARATION:
    return ((const FunctionDeclaration *)decl->data)->name;
  case AST_STRUCT_DECLARATION:
    return ((const StructDeclaration *)decl->data)->name;
  case AST_VAR_DECLARATION:
    return ((const VarDeclaration *)decl->data)->name;
  default:
    return NULL;
  }
}

static void mono_decl_index_put(MonoContext *ctx, ASTNode *decl) {
  const char *name = mono_decl_index_name(decl);
  size_t mask = ctx->decl_slot_count - 1;
  size_t slot;
  if (!name) {
    return;
  }
  slot = mettle_fnv1a_hash(name) & mask;
  while (ctx->decl_slots[slot]) {
    slot = (slot + 1) & mask;
  }
  ctx->decl_slots[slot] = decl;
}

static int mono_decl_index_sync(MonoContext *ctx) {
  size_t count = ctx->module ? ctx->module->declaration_count : 0;
  if (count == 0) {
    return 0;
  }
  if (count * 2 > ctx->decl_slot_count) {
    size_t slots = ctx->decl_slot_count ? ctx->decl_slot_count : 256;
    ASTNode **grown;
    while (count * 4 > slots) {
      slots *= 2;
    }
    grown = calloc(slots, sizeof(ASTNode *));
    if (!grown) {
      return 0;
    }
    free(ctx->decl_slots);
    ctx->decl_slots = grown;
    ctx->decl_slot_count = slots;
    ctx->decls_indexed = 0;
  }
  while (ctx->decls_indexed < count) {
    mono_decl_index_put(ctx, ctx->module->declarations[ctx->decls_indexed++]);
  }
  return 1;
}

static ASTNode *mono_find_declaration(MonoContext *ctx, ASTNodeType type,
                                      const char *name, size_t *cursor) {
  size_t mask;
  size_t slot;
  if (!name || !mono_decl_index_sync(ctx)) {
    return NULL;
  }
  mask = ctx->decl_slot_count - 1;
  slot = *cursor ? *cursor : (mettle_fnv1a_hash(name) & mask) + 1;
  for (slot -= 1; ctx->decl_slots[slot]; slot = (slot + 1) & mask) {
    ASTNode *decl = ctx->decl_slots[slot];
    if (decl->type == type && strcmp(mono_decl_index_name(decl), name) == 0) {
      *cursor = ((slot + 1) & mask) + 1;
      return decl;
    }
  }
  return NULL;
}

static char *mono_declared_return_text(const char *return_type) {
  if (!return_type || strcmp(return_type, "void") == 0) {
    return NULL;
  }
  return mettle_strdup(return_type);
}

static char *mono_function_return_text(MonoContext *ctx, const char *name) {
  size_t cursor = 0;
  ASTNode *decl;
  char *found = NULL;
  while ((decl = mono_find_declaration(ctx, AST_FUNCTION_DECLARATION, name,
                                       &cursor)) != NULL) {
    FunctionDeclaration *fd = (FunctionDeclaration *)decl->data;
    char *returned = mono_declared_return_text(fd->return_type);
    if (fd->type_param_count > 0 || fd->meta_role != META_ROLE_NONE ||
        !returned || (found && strcmp(found, returned) != 0)) {
      free(returned);
      free(found);
      return NULL;
    }
    free(found);
    found = returned;
  }
  return found;
}

static int mono_is_word_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_';
}

static char *mono_substitute_written(const char *text, char **params,
                                     char **args, size_t count) {
  char *out = NULL;
  for (int pass = 0; pass < 2; pass++) {
    const char *p = text;
    size_t used = 0;
    while (*p) {
      size_t run = 0;
      const char *replacement = NULL;
      size_t copied;
      while (mono_is_word_char(p[run])) {
        run++;
      }
      for (size_t i = 0; run > 0 && i < count && !replacement; i++) {
        if (strlen(params[i]) == run && strncmp(p, params[i], run) == 0) {
          replacement = args[i];
        }
      }
      if (run == 0) {
        run = 1;
      }
      copied = replacement ? strlen(replacement) : run;
      if (out) {
        memcpy(out + used, replacement ? replacement : p, copied);
      }
      used += copied;
      p += run;
    }
    if (out) {
      out[used] = '\0';
    } else {
      out = malloc(used + 1);
      if (!out) {
        return NULL;
      }
    }
  }
  return out;
}

static char *mono_written_form(MonoContext *ctx, char *text) {
  size_t core = 0;
  while (text && text[core] && text[core] != '*' && text[core] != '[') {
    core++;
  }
  for (size_t i = 0; core > 0 && i < ctx->instance_count; i++) {
    const Instantiation *instance = &ctx->instances[i];
    char *written;
    char *joined;
    size_t size;
    if (!instance->mangled_name || strlen(instance->mangled_name) != core ||
        strncmp(instance->mangled_name, text, core) != 0) {
      continue;
    }
    written = written_generic_name(instance->generic_name, instance->type_args,
                                   instance->type_arg_count);
    if (!written) {
      return text;
    }
    size = strlen(written) + strlen(text + core) + 1;
    joined = malloc(size);
    if (joined) {
      snprintf(joined, size, "%s%s", written, text + core);
      free(text);
      text = joined;
    }
    free(written);
    return text;
  }
  return text;
}

static char *mono_generic_return_text(MonoContext *ctx, CallExpression *call) {
  GenericDef *def = find_generic_def(ctx, call->function_name);
  FunctionDeclaration *fd;
  if (!def || def->is_struct || !def->node || call->type_arg_count == 0 ||
      call->type_arg_count != def->type_param_count) {
    return NULL;
  }
  fd = (FunctionDeclaration *)def->node->data;
  if (!fd || !fd->return_type || strcmp(fd->return_type, "void") == 0) {
    return NULL;
  }
  return mono_substitute_written(fd->return_type, def->type_params,
                                 call->type_args, def->type_param_count);
}

static char *mono_without_pointer(const char *text) {
  char *base = text ? mettle_strdup(text) : NULL;
  size_t len = base ? strlen(base) : 0;
  if (len > 0 && base[len - 1] == '*') {
    base[len - 1] = '\0';
  }
  return base;
}

typedef struct {
  StructDeclaration *sd;
  GenericDef *def;
  char **args;
  size_t arg_count;
  char *parsed_base;
  char **parsed_args;
} MonoStructView;

static int mono_struct_view(MonoContext *ctx, const char *base,
                            MonoStructView *view) {
  size_t cursor = 0;
  ASTNode *decl =
      mono_find_declaration(ctx, AST_STRUCT_DECLARATION, base, &cursor);
  memset(view, 0, sizeof(*view));
  if (decl && ((StructDeclaration *)decl->data)->type_param_count == 0) {
    view->sd = (StructDeclaration *)decl->data;
    return 1;
  }
  if (parse_generic_type_name(base, &view->parsed_base, &view->parsed_args,
                              &view->arg_count)) {
    view->def = find_generic_def(ctx, view->parsed_base);
    view->args = view->parsed_args;
  } else {
    for (size_t i = 0; i < ctx->instance_count; i++) {
      if (ctx->instances[i].mangled_name &&
          strcmp(ctx->instances[i].mangled_name, base) == 0) {
        view->def = find_generic_def(ctx, ctx->instances[i].generic_name);
        view->args = ctx->instances[i].type_args;
        view->arg_count = ctx->instances[i].type_arg_count;
        break;
      }
    }
  }
  if (view->def && view->def->is_struct && view->def->node &&
      view->arg_count == view->def->type_param_count) {
    view->sd = (StructDeclaration *)view->def->node->data;
  }
  return view->sd != NULL;
}

static void mono_struct_view_free(MonoStructView *view) {
  for (size_t a = 0; view->parsed_args && a < view->arg_count; a++) {
    free(view->parsed_args[a]);
  }
  free(view->parsed_args);
  free(view->parsed_base);
}

static char *mono_struct_view_text(const MonoStructView *view,
                                   const char *declared) {
  if (!view->def) {
    return mettle_strdup(declared);
  }
  return mono_substitute_written(declared, view->def->type_params, view->args,
                                 view->arg_count);
}

static char *mono_method_return_text(MonoContext *ctx, const char *receiver,
                                     const char *method) {
  char *base = mono_without_pointer(receiver);
  char *lifted = NULL;
  char *found = NULL;
  MonoStructView view;
  size_t len;
  if (!base) {
    return NULL;
  }
  if (mono_struct_view(ctx, base, &view)) {
    for (size_t m = 0; m < view.sd->method_count; m++) {
      FunctionDeclaration *md = (FunctionDeclaration *)view.sd->methods[m]->data;
      if (md && md->name && strcmp(md->name, method) == 0) {
        found = md->return_type && strcmp(md->return_type, "void") != 0
                    ? mono_struct_view_text(&view, md->return_type)
                    : NULL;
        mono_struct_view_free(&view);
        free(base);
        return found;
      }
    }
  }
  mono_struct_view_free(&view);
  len = strlen(base) + strlen(method) + 2;
  lifted = malloc(len);
  if (lifted) {
    snprintf(lifted, len, "%s_%s", base, method);
    found = mono_function_return_text(ctx, lifted);
    free(lifted);
  }
  free(base);
  return found;
}

static char *mono_pointer_call_return_text(const char *type) {
  int depth = 0;
  if (!type ||
      (strncmp(type, "fn(", 3) != 0 && strncmp(type, "Fn(", 3) != 0)) {
    return NULL;
  }
  for (const char *p = type + 2; *p; p++) {
    if (*p == '(') {
      depth++;
    } else if (*p == ')' && --depth == 0) {
      p++;
      while (*p == ' ') {
        p++;
      }
      if (p[0] != '-' || p[1] != '>') {
        return NULL;
      }
      p += 2;
      while (*p == ' ') {
        p++;
      }
      return mono_declared_return_text(p);
    }
  }
  return NULL;
}

static char *mono_static_type_of(MonoContext *ctx, ASTNode *expr,
                                 MonoVarEnv *env);

static char *mono_static_return_type_of_call(MonoContext *ctx, ASTNode *expr,
                                             MonoVarEnv *env) {
  CallExpression *call = (CallExpression *)expr->data;
  const MonoVarBinding *bound;
  char *generic;
  if (!call || !call->function_name || !ctx->module) {
    return NULL;
  }
  if (call->object) {
    char *receiver = mono_static_type_of(ctx, call->object, env);
    char *returned =
        mono_method_return_text(ctx, receiver, call->function_name);
    free(receiver);
    return returned;
  }
  bound = mono_env_find(env, call->function_name);
  if (bound) {
    return mono_pointer_call_return_text(bound->type_name);
  }
  generic = mono_generic_return_text(ctx, call);
  return generic ? generic : mono_function_return_text(ctx, call->function_name);
}

static char *mono_field_type_text(MonoContext *ctx, const char *owner,
                                  const char *field) {
  char *base = mono_without_pointer(owner);
  char *found = NULL;
  MonoStructView view;
  size_t base_len = base ? strlen(base) : 0;
  if (!base) {
    return NULL;
  }
  if (strcmp(field, "length") == 0 &&
      (strcmp(base, "string") == 0 ||
       (base_len > 2 && strcmp(base + base_len - 2, "[]") == 0))) {
    found = mettle_strdup(strcmp(base, "string") == 0 ? "uint64" : "int64");
    free(base);
    return found;
  }
  if (mono_struct_view(ctx, base, &view)) {
    for (size_t f = 0; f < view.sd->field_count; f++) {
      if (view.sd->field_names[f] &&
          strcmp(view.sd->field_names[f], field) == 0) {
        found = mono_struct_view_text(&view, view.sd->field_types[f]);
        break;
      }
    }
  }
  mono_struct_view_free(&view);
  free(base);
  return found;
}

static int mono_is_number_type(const char *type) {
  static const char *const NUMBERS[] = {"int8",   "int16",  "int32",   "int64",
                                        "uint8",  "uint16", "uint32",  "uint64",
                                        "float32", "float64"};
  for (size_t i = 0; i < sizeof(NUMBERS) / sizeof(NUMBERS[0]); i++) {
    if (strcmp(type, NUMBERS[i]) == 0) {
      return 1;
    }
  }
  return 0;
}

static int mono_is_int32_literal(const ASTNode *node) {
  const NumberLiteral *literal = NULL;
  if (node && node->type == AST_UNARY_EXPRESSION && node->data) {
    const UnaryExpression *unary = (const UnaryExpression *)node->data;
    if (unary->operator && strcmp(unary->operator, "-") == 0) {
      node = unary->operand;
    }
  }
  literal = node && node->type == AST_NUMBER_LITERAL
                ? (const NumberLiteral *)node->data
                : NULL;
  return literal && !literal->is_float && !literal->is_char &&
         strcmp(ast_integer_literal_type_name(literal), "int32") == 0;
}

static int mono_outranks_int32(const char *type) {
  return strcmp(type, "int64") == 0 || strcmp(type, "uint64") == 0 ||
         strcmp(type, "float32") == 0 || strcmp(type, "float64") == 0;
}

static int mono_is_integer_text(const char *type) {
  return mono_is_number_type(type) && strncmp(type, "float", 5) != 0;
}

static char *mono_exact_binary_type(MonoContext *ctx, BinaryExpression *bin,
                                    MonoVarEnv *env) {
  const char *op = bin->operator;
  int is_shift = strcmp(op, "<<") == 0 || strcmp(op, ">>") == 0;
  int is_bits = is_shift || strcmp(op, "&") == 0 || strcmp(op, "|") == 0 ||
                strcmp(op, "^") == 0;
  char *left;
  char *right;
  char *result = NULL;
  if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 || strcmp(op, "<") == 0 ||
      strcmp(op, ">") == 0 || strcmp(op, "<=") == 0 || strcmp(op, ">=") == 0 ||
      strcmp(op, "&&") == 0 || strcmp(op, "||") == 0) {
    return mettle_strdup("bool");
  }
  left = mono_static_type_of(ctx, bin->left, env);
  right = mono_static_type_of(ctx, bin->right, env);
  if (left && right) {
    size_t left_len = strlen(left);
    if (is_shift) {
      result = mono_is_integer_text(left) ? left : NULL;
    } else if (strcmp(left, right) == 0) {
      result = mono_is_number_type(left) ||
                       (strcmp(left, "string") == 0 && strcmp(op, "+") == 0)
                   ? left
                   : NULL;
    } else if (left_len > 0 && left[left_len - 1] == '*') {
      result = (strcmp(op, "+") == 0 || strcmp(op, "-") == 0) &&
                       mono_is_integer_text(right)
                   ? left
                   : NULL;
    } else if (mono_is_int32_literal(bin->right)) {
      result = mono_is_number_type(left) &&
                       (is_bits || mono_outranks_int32(left) ||
                        strcmp(left, "uint32") == 0)
                   ? left
                   : NULL;
    } else if (mono_is_int32_literal(bin->left)) {
      result = !is_bits && mono_outranks_int32(right) ? right : NULL;
    }
  }
  if (result != left) {
    free(left);
  }
  if (result != right) {
    free(right);
  }
  return result;
}

static char *mono_exact_unary_type(MonoContext *ctx, UnaryExpression *un,
                                   MonoVarEnv *env) {
  char *inner;
  size_t len;
  if (strcmp(un->operator, "!") == 0) {
    return mettle_strdup("bool");
  }
  inner = mono_static_type_of(ctx, un->operand, env);
  len = inner ? strlen(inner) : 0;
  if (len == 0) {
    free(inner);
    return NULL;
  }
  if (strcmp(un->operator, "&") == 0) {
    char *text;
    if (inner[len - 1] == ']') {
      free(inner);
      return NULL;
    }
    text = malloc(len + 2);
    if (text) {
      snprintf(text, len + 2, "%s*", inner);
    }
    free(inner);
    return text;
  }
  if (strcmp(un->operator, "*") == 0) {
    if (inner[len - 1] == '*') {
      inner[len - 1] = '\0';
      return inner;
    }
    free(inner);
    return NULL;
  }
  if (mono_is_number_type(inner) &&
      !(un->operand && un->operand->type == AST_NUMBER_LITERAL &&
        strcmp(inner, "uint64") == 0)) {
    return inner;
  }
  free(inner);
  return NULL;
}

static char *mono_exact_index_type(MonoContext *ctx, ArrayIndexExpression *ix,
                                   MonoVarEnv *env) {
  char *base = mono_static_type_of(ctx, ix->array, env);
  size_t len = base ? strlen(base) : 0;
  if (base && strcmp(base, "string") == 0) {
    free(base);
    return mettle_strdup("char");
  }
  if (len > 0 && base[len - 1] == '*') {
    base[len - 1] = '\0';
    return base;
  }
  if (len > 0 && base[len - 1] == ']') {
    char *open = strchr(base, '[');
    if (open && open == strrchr(base, '[') && !strchr(open, ',')) {
      *open = '\0';
      return base;
    }
  }
  free(base);
  return NULL;
}

static char *mono_identifier_type(MonoContext *ctx, Identifier *id,
                                  MonoVarEnv *env) {
  const MonoVarBinding *bound = id ? mono_env_find(env, id->name) : NULL;
  size_t cursor = 0;
  ASTNode *global;
  if (bound) {
    return bound->type_name ? mettle_strdup(bound->type_name) : NULL;
  }
  if (!id || !id->name) {
    return NULL;
  }
  if (strcmp(id->name, "true") == 0 || strcmp(id->name, "false") == 0) {
    return mettle_strdup("bool");
  }
  global = mono_find_declaration(ctx, AST_VAR_DECLARATION, id->name, &cursor);
  if (global) {
    VarDeclaration *declared = (VarDeclaration *)global->data;
    if (declared->type_name) {
      return mettle_strdup(declared->type_name);
    }
    if (declared->initializer &&
        (declared->initializer->type == AST_NUMBER_LITERAL ||
         declared->initializer->type == AST_STRING_LITERAL)) {
      return mono_static_type_of(ctx, declared->initializer, NULL);
    }
  }
  return NULL;
}

static char *mono_new_expression_type(const NewExpression *ne) {
  char *text;
  size_t len;
  if (!ne || !ne->type_name || ne->extent_count > 0) {
    return NULL;
  }
  len = strlen(ne->type_name) + 3;
  text = malloc(len);
  if (text) {
    snprintf(text, len, ne->count ? "%s[]" : "%s*", ne->type_name);
  }
  return text;
}

static char *mono_static_type_of(MonoContext *ctx, ASTNode *expr,
                                 MonoVarEnv *env) {
  if (!expr) {
    return NULL;
  }
  switch (expr->type) {
  case AST_NUMBER_LITERAL: {
    NumberLiteral *lit = (NumberLiteral *)expr->data;
    if (!lit) {
      return NULL;
    }
    if (lit->is_char) {
      return mettle_strdup("char");
    }
    return mettle_strdup(lit->is_float ? "float64"
                                       : ast_integer_literal_type_name(lit));
  }
  case AST_STRING_LITERAL:
    return mettle_strdup("string");
  case AST_IDENTIFIER:
    return mono_identifier_type(ctx, (Identifier *)expr->data, env);
  case AST_CAST_EXPRESSION: {
    CastExpression *cast = (CastExpression *)expr->data;
    return cast && cast->type_name ? mettle_strdup(cast->type_name) : NULL;
  }
  case AST_NEW_EXPRESSION:
    return mono_new_expression_type((NewExpression *)expr->data);
  case AST_UNARY_EXPRESSION: {
    UnaryExpression *un = (UnaryExpression *)expr->data;
    return un && un->operator ? mono_exact_unary_type(ctx, un, env) : NULL;
  }
  case AST_INDEX_EXPRESSION: {
    ArrayIndexExpression *ix = (ArrayIndexExpression *)expr->data;
    return ix ? mono_exact_index_type(ctx, ix, env) : NULL;
  }
  case AST_MEMBER_ACCESS: {
    MemberAccess *member = (MemberAccess *)expr->data;
    char *owner = member ? mono_static_type_of(ctx, member->object, env) : NULL;
    char *field = owner ? mono_field_type_text(ctx, owner, member->member) : NULL;
    free(owner);
    return field;
  }
  case AST_BINARY_EXPRESSION: {
    BinaryExpression *bin = (BinaryExpression *)expr->data;
    return bin && bin->operator ? mono_exact_binary_type(ctx, bin, env) : NULL;
  }
  case AST_FUNCTION_CALL:
    return mono_static_return_type_of_call(ctx, expr, env);
  default:
    return NULL;
  }
}

static char *mono_argument_type_of(MonoContext *ctx, ASTNode *expr,
                                   MonoVarEnv *env) {
  char *type = mono_static_type_of(ctx, expr, env);
  if (!type && expr && expr->type == AST_BINARY_EXPRESSION) {
    BinaryExpression *bin = (BinaryExpression *)expr->data;
    type = bin ? mono_argument_type_of(ctx, bin->left, env) : NULL;
    if (!type && bin) {
      type = mono_argument_type_of(ctx, bin->right, env);
    }
  }
  return type;
}

static long mono_literal_param_index(const GenericDef *def,
                                     const char *parameter_type,
                                     const ASTNode *argument) {
  const NumberLiteral *literal = NULL;
  if (!argument || argument->type != AST_NUMBER_LITERAL ||
      !argument->data) {
    return -1;
  }
  literal = (const NumberLiteral *)argument->data;
  if (literal->is_char) {
    return -1;
  }
  for (size_t p = 0; p < def->type_param_count; p++) {
    if (strcmp(parameter_type, def->type_params[p]) == 0) {
      return (long)p;
    }
  }
  return -1;
}

static int mono_literal_fits_type(const ASTNode *argument, const char *type) {
  const NumberLiteral *literal = (const NumberLiteral *)argument->data;
  if (strncmp(type, "float", 5) == 0 || strncmp(type, "bfloat", 6) == 0) {
    return 1;
  }
  return !literal->is_float &&
         (strncmp(type, "int", 3) == 0 || strncmp(type, "uint", 4) == 0);
}

static int mono_infer_call_type_args(MonoContext *ctx, ASTNode *node,
                                     MonoVarEnv *env) {
  CallExpression *call = (CallExpression *)node->data;
  GenericDef *def = NULL;
  FunctionDeclaration *fd = NULL;
  MonoTypeBinding *bindings = NULL;
  size_t i;
  int ok = 1;

  if (!call || call->type_arg_count > 0 || !call->function_name) {
    return 1;
  }
  def = find_generic_def(ctx, call->function_name);
  if (!def || def->is_struct || def->type_param_count == 0 || !def->node) {
    return 1;
  }
  fd = (FunctionDeclaration *)def->node->data;
  if (!fd) {
    return 1;
  }

  bindings = calloc(def->type_param_count, sizeof(MonoTypeBinding));
  if (!bindings) {
    return 1;
  }
  for (i = 0; i < def->type_param_count; i++) {
    bindings[i].name = def->type_params[i];
  }

  for (int pass = 0; pass < 2; pass++) {
    for (i = 0; i < call->argument_count && i < fd->parameter_count; i++) {
      long literal_param = mono_literal_param_index(
          def, fd->parameter_types[i], call->arguments[i]);
      char *actual = NULL;
      if ((literal_param >= 0) != (pass == 1)) {
        continue;
      }
      actual = mono_written_form(
          ctx, mono_argument_type_of(ctx, call->arguments[i], env));
      if (!actual) {
        continue;
      }
      if (literal_param >= 0 && bindings[literal_param].bound &&
          mono_literal_fits_type(call->arguments[i],
                                 bindings[literal_param].bound)) {
        free(actual);
        continue;
      }
      if (!mono_match_type_text(fd->parameter_types[i], actual,
                                def->type_params, def->type_param_count,
                                bindings)) {
        char message[512];
        snprintf(message, sizeof(message),
                 "Argument %zu of '%s' asks for a different type parameter "
                 "than an earlier argument did. Name the types at the call",
                 i + 1, call->function_name);
        mono_report_error(ctx, node->location, message);
        free(actual);
        ok = 0;
        goto done;
      }
      free(actual);
    }
  }

  for (i = 0; i < def->type_param_count; i++) {
    if (!bindings[i].bound) {
      char message[512];
      snprintf(message, sizeof(message),
               "Nothing in this call says what '%s' is in '%s'. Name it at "
               "the call, as '%s<sometype>(...)'",
               bindings[i].name, call->function_name, call->function_name);
      mono_report_error(ctx, node->location, message);
      ok = 0;
      goto done;
    }
  }

  call->type_args = malloc(def->type_param_count * sizeof(char *));
  if (!call->type_args) {
    ok = 0;
    goto done;
  }
  for (i = 0; i < def->type_param_count; i++) {
    call->type_args[i] = bindings[i].bound;
    bindings[i].bound = NULL;
  }
  call->type_arg_count = def->type_param_count;

done:
  for (i = 0; i < def->type_param_count; i++) {
    mettle_free_string(bindings[i].bound);
  }
  free(bindings);
  return ok;
}

static void mono_state_binding_type(MonoContext *ctx, VarDeclaration *decl,
                                    MonoVarEnv *env) {
  char *stated;
  if (decl->type_name || decl->structural_type || !decl->initializer) {
    return;
  }
  stated = mono_static_type_of(ctx, decl->initializer, env);
  if (stated) {
    decl->type_name = (char *)string_intern(stated);
    decl->type_is_inferred = 1;
    free(stated);
  }
}

static int mono_infer_type_args_in_node(MonoContext *ctx, ASTNode *node,
                                        MonoVarEnv *env) {
  size_t i;

  if (!node) {
    return 1;
  }

  if (node->type == AST_FUNCTION_CALL) {
    CallExpression *call = (CallExpression *)node->data;
    if (call) {
      for (i = 0; i < call->argument_count; i++) {
        if (!mono_infer_type_args_in_node(ctx, call->arguments[i], env)) {
          return 0;
        }
      }
      if (call->object &&
          !mono_infer_type_args_in_node(ctx, call->object, env)) {
        return 0;
      }
    }
    if (!mono_infer_call_type_args(ctx, node, env)) {
      return 0;
    }
    return 1;
  }

  if (node->type == AST_VAR_DECLARATION) {
    VarDeclaration *decl = (VarDeclaration *)node->data;
    if (decl && decl->initializer &&
        !mono_infer_type_args_in_node(ctx, decl->initializer, env)) {
      return 0;
    }
    if (decl && decl->name) {
      mono_state_binding_type(ctx, decl, env);
      if (!mono_env_add(env, decl->name, decl->type_name)) {
        return 0;
      }
    }
    return 1;
  }

  if (node->type == AST_MATCH_STATEMENT && node->data) {
    MatchStatement *match = (MatchStatement *)node->data;
    if (!mono_infer_type_args_in_node(ctx, match->expression, env)) {
      return 0;
    }
    for (i = 0; i < match->arm_count; i++) {
      size_t scope = env->count;
      if (!mono_env_add(env, match->arms[i].binding_name, NULL) ||
          !mono_infer_type_args_in_node(ctx, match->arms[i].body, env)) {
        return 0;
      }
      env->count = scope;
    }
    return 1;
  }

  {
    size_t scope = env->count;
    if (node->type == AST_COMPTIME_FOR && node->data &&
        !mono_env_add(env, ((ComptimeForStatement *)node->data)->binding_name,
                      NULL)) {
      return 0;
    }
    if (node->type == AST_LAMBDA_EXPRESSION && node->data) {
      FunctionDeclaration *lambda = (FunctionDeclaration *)node->data;
      for (i = 0; i < lambda->parameter_count; i++) {
        if (!mono_env_add(env, lambda->parameter_names[i],
                          lambda->parameter_types[i])) {
          return 0;
        }
      }
    }
    for (i = 0; i < node->child_count; i++) {
      if (!mono_infer_type_args_in_node(ctx, node->children[i], env)) {
        return 0;
      }
    }
    if (mono_node_opens_scope(node)) {
      env->count = scope;
    }
  }
  return 1;
}

static int mono_infer_type_args_in_body(MonoContext *ctx,
                                        FunctionDeclaration *fn,
                                        char *receiver) {
  MonoVarEnv env = {0};
  int ok = 1;
  size_t i;

  if (receiver && !mono_env_add(&env, (char *)"this", receiver)) {
    ok = 0;
  }
  for (i = 0; i < fn->parameter_count && ok; i++) {
    if (!mono_env_add(&env, fn->parameter_names[i], fn->parameter_types[i])) {
      ok = 0;
    }
  }
  if (ok) {
    ok = mono_infer_type_args_in_node(ctx, fn->body, &env);
  }
  free(env.items);
  return ok;
}

static int mono_infer_type_args_in_function(MonoContext *ctx,
                                            ASTNode *function) {
  FunctionDeclaration *fn = NULL;

  if (!ctx || !function) {
    return 1;
  }
  if (function->type == AST_STRUCT_DECLARATION && function->data) {
    StructDeclaration *sd = (StructDeclaration *)function->data;
    for (size_t m = 0; sd->type_param_count == 0 && m < sd->method_count; m++) {
      FunctionDeclaration *method = (FunctionDeclaration *)sd->methods[m]->data;
      if (method && method->body &&
          !mono_infer_type_args_in_body(ctx, method, sd->name)) {
        return 0;
      }
    }
    return 1;
  }
  if (function->type != AST_FUNCTION_DECLARATION) {
    return 1;
  }
  fn = (FunctionDeclaration *)function->data;
  if (!fn || !fn->body || fn->type_param_count > 0) {
    return 1;
  }

  return mono_infer_type_args_in_body(ctx, fn, NULL);
}

static int mono_infer_type_args(MonoContext *ctx, ASTNode *program) {
  Program *prog = NULL;
  size_t i;

  if (!ctx || !program || program->type != AST_PROGRAM) {
    return 1;
  }
  prog = (Program *)program->data;
  if (!prog) {
    return 1;
  }
  for (i = 0; i < prog->declaration_count; i++) {
    if (!mono_infer_type_args_in_function(ctx, prog->declarations[i])) {
      return 0;
    }
  }
  return 1;
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

static void mono_this_to_deref(ASTNode *node) {
  if (!node) {
    return;
  }

  if (node->type == AST_IDENTIFIER) {
    Identifier *id = (Identifier *)node->data;
    if (id && id->name && strcmp(id->name, "this") == 0) {
      ASTNode *inner = ast_create_identifier("this", node->location);
      UnaryExpression *deref = inner ? malloc(sizeof(UnaryExpression)) : NULL;
      char *op = deref ? mettle_strdup("*") : NULL;
      if (!op) {
        free(deref);
        ast_destroy_node(inner);
        return;
      }
      deref->operator= op;
      deref->operand = inner;
      mettle_free_string(id->name);
      free(id);
      node->type = AST_UNARY_EXPRESSION;
      node->data = deref;
      ast_add_child(node, inner);
      return;
    }
  }

  for (size_t i = 0; i < node->child_count; i++) {
    mono_this_to_deref(node->children[i]);
  }
}

typedef struct {
  const char **names;
  size_t count;
  size_t capacity;
} MonoNameSet;

static int mono_name_set_has(const MonoNameSet *set, const char *name) {
  if (!set || !name) {
    return 0;
  }
  for (size_t i = 0; i < set->count; i++) {
    if (strcmp(set->names[i], name) == 0) {
      return 1;
    }
  }
  return 0;
}

static void mono_name_set_add(MonoNameSet *set, const char *name) {
  if (!name || mono_name_set_has(set, name)) {
    return;
  }
  if (set->count >= set->capacity) {
    size_t capacity = set->capacity ? set->capacity * 2 : 8;
    const char **grown = realloc(set->names, capacity * sizeof(const char *));
    if (!grown) {
      return;
    }
    set->names = grown;
    set->capacity = capacity;
  }
  set->names[set->count++] = name;
}

static void mono_collect_pointer_receiver_methods(ASTNode *node,
                                                  MonoNameSet *set) {
  if (!node) {
    return;
  }
  if (node->type == AST_FUNCTION_CALL) {
    CallExpression *call = (CallExpression *)node->data;
    if (call && call->function_name && call->object &&
        call->object->type == AST_UNARY_EXPRESSION) {
      UnaryExpression *unary = (UnaryExpression *)call->object->data;
      if (unary && unary->operator&& strcmp(unary->operator, "*") == 0) {
        mono_name_set_add(set, call->function_name);
      }
    }
  }
  for (size_t i = 0; i < node->child_count; i++) {
    mono_collect_pointer_receiver_methods(node->children[i], set);
  }
}

static ASTNode *mono_lift_one_method(MonoContext *ctx, const char *struct_name,
                                     ASTNode *method, int receiver_is_pointer) {
  FunctionDeclaration *md = (FunctionDeclaration *)method->data;
  const char *suffix = receiver_is_pointer ? MONO_PTR_RECEIVER_SUFFIX : "";
  size_t mangled_len =
      strlen(struct_name) + 1 + strlen(md->name) + strlen(suffix) + 1;
  char *mangled = malloc(mangled_len);
  if (!mangled) {
    mono_report_error(ctx, method->location,
                      "Out of memory lifting a struct method");
    return NULL;
  }
  snprintf(mangled, mangled_len, "%s_%s%s", struct_name, md->name, suffix);

  size_t count = md->parameter_count + 1;
  char **names = calloc(count, sizeof(char *));
  char **types = calloc(count, sizeof(char *));
  if (!names || !types) {
    free(mangled);
    free(names);
    free(types);
    mono_report_error(ctx, method->location,
                      "Out of memory lifting a struct method");
    return NULL;
  }
  names[0] = mettle_strdup("this");
  if (receiver_is_pointer) {
    size_t len = strlen(struct_name) + 2;
    types[0] = malloc(len);
    if (types[0]) {
      snprintf(types[0], len, "%s*", struct_name);
    }
  } else {
    types[0] = mettle_strdup(struct_name);
  }
  int copied_ok = names[0] != NULL && types[0] != NULL;
  for (size_t p = 0; copied_ok && p < md->parameter_count; p++) {
    names[p + 1] = mettle_strdup(md->parameter_names[p]);
    types[p + 1] = mettle_strdup(md->parameter_types[p]);
    copied_ok = names[p + 1] != NULL && types[p + 1] != NULL;
  }

  ASTNode *body = NULL;
  if (copied_ok) {
    body = receiver_is_pointer ? ast_clone_node(md->body) : md->body;
    if (body && receiver_is_pointer) {
      mono_this_to_deref(body);
    }
  }

  ASTNode *fn_node =
      body ? ast_create_node(AST_FUNCTION_DECLARATION, method->location) : NULL;
  FunctionDeclaration *fn =
      fn_node ? calloc(1, sizeof(FunctionDeclaration)) : NULL;
  if (!fn) {
    for (size_t p = 0; p < count; p++) {
      mettle_free_string(names[p]);
      mettle_free_string(types[p]);
    }
    free(names);
    free(types);
    free(mangled);
    if (body && receiver_is_pointer) {
      ast_destroy_node(body);
    }
    if (fn_node) {
      ast_destroy_node(fn_node);
    }
    mono_report_error(ctx, method->location,
                      "Out of memory lifting a struct method");
    return NULL;
  }

  fn->name = mangled;
  fn->parameter_names = names;
  fn->parameter_types = types;
  fn->parameter_count = count;
  fn->return_type = md->return_type ? mettle_strdup(md->return_type) : NULL;
  fn->body = body;
  fn->is_inline = md->is_inline;
  fn->is_inline_contract = md->is_inline_contract;
  fn->is_noinline = md->is_noinline;
  fn->is_pure = md->is_pure;
  fn->reference_twin = md->reference_twin ? strdup(md->reference_twin) : NULL;
  fn->explain_code = md->explain_code ? strdup(md->explain_code) : NULL;
  fn->explain_text = md->explain_text ? strdup(md->explain_text) : NULL;
  fn->is_noalloc = md->is_noalloc;
  fn->is_swappable = md->is_swappable;
  fn->is_naked = md->is_naked;
  fn->is_interrupt = md->is_interrupt;
  fn->simd_mode = md->simd_mode;
  if (!receiver_is_pointer) {
    md->body = NULL;
  }
  fn_node->data = fn;
  ast_add_child(fn_node, fn->body);
  return fn_node;
}

static int mono_lift_struct_methods(MonoContext *ctx, Program *prog,
                                    ASTNode *program) {
  MonoNameSet pointer_methods = {0};
  int ok = 1;
  size_t original_count = prog->declaration_count;

  mono_collect_pointer_receiver_methods(program, &pointer_methods);

  for (size_t d = 0; d < original_count; d++) {
    ASTNode *decl = prog->declarations[d];
    if (!decl || decl->type != AST_STRUCT_DECLARATION) {
      continue;
    }
    StructDeclaration *sd = (StructDeclaration *)decl->data;
    if (!sd || !sd->name || sd->method_count == 0) {
      continue;
    }
    if (sd->type_param_count > 0) {
      continue;
    }

    for (size_t m = 0; m < sd->method_count; m++) {
      ASTNode *method = sd->methods[m];
      if (!method || !method->data) {
        continue;
      }
      FunctionDeclaration *md = (FunctionDeclaration *)method->data;
      if (!md || !md->name || !md->body) {
        continue;
      }

      for (int ptr = 1; ptr >= 0; ptr--) {
        ASTNode *fn_node = NULL;
        ASTNode **grown = NULL;

        if (ptr && !mono_name_set_has(&pointer_methods, md->name)) {
          continue;
        }

        fn_node = mono_lift_one_method(ctx, sd->name, method, ptr);
        if (!fn_node) {
          ok = 0;
          goto done;
        }

        grown = realloc(prog->declarations,
                        (prog->declaration_count + 1) * sizeof(ASTNode *));
        if (!grown) {
          ast_destroy_node(fn_node);
          mono_report_error(ctx, method->location,
                            "Failed to append a lifted struct method");
          ok = 0;
          goto done;
        }
        prog->declarations = grown;
        prog->declarations[prog->declaration_count++] = fn_node;
        ast_add_child(program, fn_node);
      }
    }
  }

done:
  free(pointer_methods.names);
  return ok;
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

  ctx->instances[index].emitted = 1;
  if (!mono_infer_type_args_in_function(ctx, mono_node)) {
    *success = 0;
    return 0;
  }
  collect_type_instantiations(mono_node, ctx);
  return 1;
}

typedef struct {
  char **names;
  char **types;
  size_t count;
  size_t cap;
} CCEnv;

static int cc_env_has(const CCEnv *e, const char *name) {
  if (!name)
    return 0;
  for (size_t i = 0; i < e->count; i++)
    if (strcmp(e->names[i], name) == 0)
      return 1;
  return 0;
}

static const char *cc_env_type(const CCEnv *e, const char *name) {
  if (!name)
    return NULL;
  for (size_t i = 0; i < e->count; i++)
    if (strcmp(e->names[i], name) == 0)
      return e->types[i];
  return NULL;
}

static void cc_env_add(CCEnv *e, const char *name, const char *type) {
  if (!name || cc_env_has(e, name))
    return;
  if (e->count == e->cap) {
    size_t ncap = e->cap ? e->cap * 2 : 8;
    char **gn = realloc(e->names, ncap * sizeof(char *));
    char **gt = realloc(e->types, ncap * sizeof(char *));
    if (!gn || !gt) {
      free(gn);
      free(gt);
      return;
    }
    e->names = gn;
    e->types = gt;
    e->cap = ncap;
  }
  e->names[e->count] = strdup(name);
  e->types[e->count] = type ? strdup(type) : NULL;
  e->count++;
}

static void cc_env_free(CCEnv *e) {
  for (size_t i = 0; i < e->count; i++) {
    free(e->names[i]);
    free(e->types[i]);
  }
  free(e->names);
  free(e->types);
  e->names = NULL;
  e->types = NULL;
  e->count = 0;
  e->cap = 0;
}

static void cc_collect_vars(ASTNode *node, CCEnv *env) {
  if (!node || node->type == AST_LAMBDA_EXPRESSION)
    return;
  if (node->type == AST_VAR_DECLARATION) {
    VarDeclaration *vd = (VarDeclaration *)node->data;
    if (vd)
      cc_env_add(env, vd->name, vd->type_name);
  }
  for (size_t i = 0; i < node->child_count; i++)
    cc_collect_vars(node->children[i], env);
}

static void cc_collect_fn_env(FunctionDeclaration *fd, CCEnv *env) {
  for (size_t i = 0; i < fd->parameter_count; i++)
    cc_env_add(env, fd->parameter_names[i], fd->parameter_types[i]);
  cc_collect_vars(fd->body, env);
}

static void cc_scan_captures(ASTNode *node, const CCEnv *enclosing,
                             const CCEnv *bound, CCEnv *caps) {
  if (!node || node->type == AST_LAMBDA_EXPRESSION)
    return;
  const char *use = NULL;
  if (node->type == AST_IDENTIFIER) {
    Identifier *id = (Identifier *)node->data;
    use = id ? id->name : NULL;
  } else if (node->type == AST_ASSIGNMENT) {
    Assignment *a = (Assignment *)node->data;
    use = a ? a->variable_name : NULL;
  } else if (node->type == AST_FUNCTION_CALL) {
    CallExpression *c = (CallExpression *)node->data;
    use = c ? c->function_name : NULL;
  }
  if (use && !cc_env_has(bound, use) && cc_env_has(enclosing, use))
    cc_env_add(caps, use, cc_env_type(enclosing, use));
  for (size_t i = 0; i < node->child_count; i++)
    cc_scan_captures(node->children[i], enclosing, bound, caps);
}

typedef struct {
  char *data;
  size_t len;
  size_t cap;
} CCBuf;

static void cc_buf_add(CCBuf *b, const char *s) {
  size_t n = strlen(s);
  if (b->len + n + 1 > b->cap) {
    size_t ncap = b->cap ? b->cap * 2 : 256;
    while (ncap < b->len + n + 1)
      ncap *= 2;
    char *grown = realloc(b->data, ncap);
    if (!grown)
      return;
    b->data = grown;
    b->cap = ncap;
  }
  memcpy(b->data + b->len, s, n + 1);
  b->len += n;
}

static void cc_buf_add_num(CCBuf *b, const char *prefix, size_t n) {
  char tmp[32];
  snprintf(tmp, sizeof(tmp), "%s%zu", prefix, n);
  cc_buf_add(b, tmp);
}

static int cc_cap_index(const CCEnv *caps, const char *name) {
  if (!name)
    return -1;
  for (size_t i = 0; i < caps->count; i++)
    if (strcmp(caps->names[i], name) == 0)
      return (int)i;
  return -1;
}

static ASTNode *cc_env_field_access(int idx, SourceLocation loc) {
  char field[32];
  snprintf(field, sizeof(field), "__cap%d", idx);
  ASTNode *env_id = ast_create_identifier("__env", loc);
  return env_id ? ast_create_member_access(env_id, field, loc) : NULL;
}

static void cc_rewrite_captures(ASTNode *node, const CCEnv *caps) {
  if (!node || node->type == AST_LAMBDA_EXPRESSION)
    return;

  if (node->type == AST_IDENTIFIER) {
    Identifier *id = (Identifier *)node->data;
    int idx = id ? cc_cap_index(caps, id->name) : -1;
    if (idx >= 0) {
      char field[32];
      snprintf(field, sizeof(field), "__cap%d", idx);
      ASTNode *env_id = ast_create_identifier("__env", node->location);
      MemberAccess *ma = env_id ? malloc(sizeof(MemberAccess)) : NULL;
      if (ma) {
        ma->object = env_id;
        ma->member = (char *)string_intern(field);
        node->type = AST_MEMBER_ACCESS;
        node->data = ma;
        node->resolved_type = NULL;
        ast_add_child(node, env_id);
      }
    }
    return;
  }

  if (node->type == AST_FUNCTION_CALL) {
    CallExpression *c = (CallExpression *)node->data;
    int idx = (c && !c->object) ? cc_cap_index(caps, c->function_name) : -1;
    if (idx >= 0) {
      ASTNode *member = cc_env_field_access(idx, node->location);
      FuncPtrCall *fp = member ? malloc(sizeof(FuncPtrCall)) : NULL;
      if (fp) {
        fp->function = member;
        fp->arguments = c->arguments;
        fp->argument_count = c->argument_count;
        fp->effect_signature = NULL;
        node->child_count = 0;
        node->type = AST_FUNC_PTR_CALL;
        node->data = fp;
        node->resolved_type = NULL;
        ast_add_child(node, member);
        for (size_t i = 0; i < fp->argument_count; i++)
          if (fp->arguments[i])
            ast_add_child(node, fp->arguments[i]);
      }
    }
  } else if (node->type == AST_ASSIGNMENT) {
    Assignment *a = (Assignment *)node->data;
    if (a && !a->target && a->variable_name) {
      int idx = cc_cap_index(caps, a->variable_name);
      if (idx >= 0) {
        ASTNode *member = cc_env_field_access(idx, node->location);
        if (member) {
          a->target = member;
          a->variable_name = NULL;
          ast_add_child(node, member);
        }
      }
    }
  }

  for (size_t i = 0; i < node->child_count; i++)
    cc_rewrite_captures(node->children[i], caps);
}

static int g_cc_counter;
static int g_closures_continue = 0;
static Program *g_adapt_whole = NULL;

static void cc_emit_capturing(ASTNode *lambda, FunctionDeclaration *fd,
                              CCEnv *caps, ASTNode *program, Program *prog,
                              ErrorReporter *reporter, int *had_error) {
  for (size_t i = 0; i < caps->count; i++) {
    if (!caps->types[i]) {
      char message[256];
      snprintf(message, sizeof(message),
               "cannot capture '%s' in a closure: a closure is built before "
               "types are checked, and its initializer does not state its type "
               "that early; write the type on the variable",
               caps->names[i]);
      if (reporter)
        error_reporter_add_error(reporter, ERROR_SEMANTIC, lambda->location,
                                 message);
      *had_error = 1;
      return;
    }
  }

  int id = g_cc_counter++;
  char env_name[32], lam_name[32], make_name[32];
  snprintf(env_name, sizeof(env_name), "__ClosEnv_%d", id);
  snprintf(lam_name, sizeof(lam_name), "__lam_%d", id);
  snprintf(make_name, sizeof(make_name), "__make_lam_%d", id);
  const char *ret = fd->return_type ? fd->return_type : "void";

  CCBuf src = {0};
  cc_buf_add(&src, "struct ");
  cc_buf_add(&src, env_name);
  cc_buf_add(&src, " {\n  __code: fn(");
  cc_buf_add(&src, env_name);
  cc_buf_add(&src, "*");
  for (size_t i = 0; i < fd->parameter_count; i++) {
    cc_buf_add(&src, ", ");
    cc_buf_add(&src, fd->parameter_types[i]);
  }
  cc_buf_add(&src, ") -> ");
  cc_buf_add(&src, ret);
  cc_buf_add(&src, ";\n");
  for (size_t i = 0; i < caps->count; i++) {
    cc_buf_add(&src, "  ");
    cc_buf_add_num(&src, "__cap", i);
    cc_buf_add(&src, ": ");
    cc_buf_add(&src, caps->types[i]);
    cc_buf_add(&src, ";\n");
  }
  cc_buf_add(&src, "}\n");

  cc_buf_add(&src, "fn ");
  cc_buf_add(&src, lam_name);
  cc_buf_add(&src, "(__env: ");
  cc_buf_add(&src, env_name);
  cc_buf_add(&src, "*");
  for (size_t i = 0; i < fd->parameter_count; i++) {
    cc_buf_add(&src, ", ");
    cc_buf_add(&src, fd->parameter_names[i]);
    cc_buf_add(&src, ": ");
    cc_buf_add(&src, fd->parameter_types[i]);
  }
  cc_buf_add(&src, ") -> ");
  cc_buf_add(&src, ret);
  cc_buf_add(&src, " {\n}\n");

  cc_buf_add(&src, "fn ");
  cc_buf_add(&src, make_name);
  cc_buf_add(&src, "(");
  for (size_t i = 0; i < caps->count; i++) {
    cc_buf_add_num(&src, "__arg", i);
    cc_buf_add(&src, ": ");
    cc_buf_add(&src, caps->types[i]);
    if (i + 1 < caps->count)
      cc_buf_add(&src, ", ");
  }
  cc_buf_add(&src, ") -> ");
  cc_buf_add(&src, env_name);
  cc_buf_add(&src, "* {\n  var __e: ");
  cc_buf_add(&src, env_name);
  cc_buf_add(&src, "* = new ");
  cc_buf_add(&src, env_name);
  cc_buf_add(&src, ";\n  __e.__code = &");
  cc_buf_add(&src, lam_name);
  cc_buf_add(&src, ";\n");
  for (size_t i = 0; i < caps->count; i++) {
    cc_buf_add(&src, "  __e.");
    cc_buf_add_num(&src, "__cap", i);
    cc_buf_add(&src, " = ");
    cc_buf_add_num(&src, "__arg", i);
    cc_buf_add(&src, ";\n");
  }
  cc_buf_add(&src, "  return __e;\n}\n");

  if (!src.data) {
    *had_error = 1;
    return;
  }

  Lexer *lx = lexer_create(src.data);
  Parser *ps = lx ? parser_create(lx) : NULL;
  ASTNode *sub = ps ? parser_parse_program(ps) : NULL;
  Program *subp = (sub && sub->type == AST_PROGRAM) ? (Program *)sub->data : NULL;
  if (!subp || subp->declaration_count < 3) {
    if (reporter)
      error_reporter_add_error(reporter, ERROR_SEMANTIC, lambda->location,
                               "internal: failed to synthesize closure");
    *had_error = 1;
    free(src.data);
    return;
  }

  ASTNode *lifted = subp->declarations[1];
  ASTNode *body = fd->body;
  fd->body = NULL;
  if (body) {
    for (size_t i = 0; i < lambda->child_count; i++) {
      if (lambda->children[i] == body) {
        for (size_t j = i + 1; j < lambda->child_count; j++)
          lambda->children[j - 1] = lambda->children[j];
        lambda->child_count--;
        break;
      }
    }
  }
  if (body && body->type == AST_PROGRAM && lifted &&
      lifted->type == AST_FUNCTION_DECLARATION) {
    FunctionDeclaration *lf = (FunctionDeclaration *)lifted->data;
    Program *lbody = lf->body ? (Program *)lf->body->data : NULL;
    Program *obody = (Program *)body->data;
    if (lbody && obody && obody->declaration_count > 0) {
      ASTNode **merged =
          realloc(lbody->declarations,
                  (lbody->declaration_count + obody->declaration_count) *
                      sizeof(ASTNode *));
      if (merged) {
        lbody->declarations = merged;
        for (size_t i = 0; i < obody->declaration_count; i++) {
          ASTNode *stmt = obody->declarations[i];
          lbody->declarations[lbody->declaration_count++] = stmt;
          ast_add_child(lf->body, stmt);
        }
        obody->declaration_count = 0;
        body->child_count = 0;
        for (size_t i = 0; i < lbody->declaration_count; i++)
          cc_rewrite_captures(lbody->declarations[i], caps);
      }
    }
  }
  if (body)
    ast_destroy_node(body);

  for (size_t i = 0; i < subp->declaration_count; i++) {
    ASTNode *d = subp->declarations[i];
    ASTNode **grown =
        realloc(prog->declarations,
                (prog->declaration_count + 1) * sizeof(ASTNode *));
    if (!grown) {
      *had_error = 1;
      break;
    }
    prog->declarations = grown;
    prog->declarations[prog->declaration_count++] = d;
    ast_add_child(program, d);
  }

  fd->name = (char *)string_intern(make_name);
  fd->env_struct_name = (char *)string_intern(env_name);
  fd->captured_count = caps->count;
  fd->captured_names = malloc(caps->count * sizeof(char *));
  fd->captured_types = malloc(caps->count * sizeof(char *));
  for (size_t i = 0; i < caps->count; i++) {
    fd->captured_names[i] = (char *)string_intern(caps->names[i]);
    fd->captured_types[i] = (char *)string_intern(caps->types[i]);
  }

  free(src.data);
}

static void cc_lift_lambda(ASTNode *lambda, ASTNode *program, Program *prog,
                           const CCEnv *enclosing, ErrorReporter *reporter,
                           int *had_error) {
  FunctionDeclaration *fd = (FunctionDeclaration *)lambda->data;
  if (!fd || fd->name)
    return;

  CCEnv bound = {0};
  cc_collect_fn_env(fd, &bound);
  CCEnv caps = {0};
  cc_scan_captures(fd->body, enclosing, &bound, &caps);
  cc_env_free(&bound);

  if (caps.count > 0) {
    cc_emit_capturing(lambda, fd, &caps, program, prog, reporter, had_error);
    cc_env_free(&caps);
    return;
  }
  cc_env_free(&caps);

  char name[32];
  snprintf(name, sizeof(name), "__lam_%d", g_cc_counter++);

  ASTNode *body = fd->body;
  fd->body = NULL;
  if (body) {
    for (size_t i = 0; i < lambda->child_count; i++) {
      if (lambda->children[i] == body) {
        for (size_t j = i + 1; j < lambda->child_count; j++)
          lambda->children[j - 1] = lambda->children[j];
        lambda->child_count--;
        break;
      }
    }
  }

  ASTNode *fn = ast_create_function_declaration(
      name, fd->parameter_names, fd->parameter_types, fd->parameter_count,
      fd->return_type, body, lambda->location);
  if (!fn) {
    *had_error = 1;
    if (body)
      ast_destroy_node(body);
    return;
  }

  ASTNode **grown =
      realloc(prog->declarations,
              (prog->declaration_count + 1) * sizeof(ASTNode *));
  if (!grown) {
    *had_error = 1;
    ast_destroy_node(fn);
    return;
  }
  prog->declarations = grown;
  prog->declarations[prog->declaration_count++] = fn;
  ast_add_child(program, fn);

  fd->name = (char *)string_intern(name);
}

static void cc_walk(ASTNode *node, const CCEnv *enclosing, ASTNode *program,
                    Program *prog, ErrorReporter *reporter, int *had_error) {
  if (!node)
    return;
  if (node->type == AST_LAMBDA_EXPRESSION) {
    cc_lift_lambda(node, program, prog, enclosing, reporter, had_error);
    return;
  }
  for (size_t i = 0; i < node->child_count; i++)
    cc_walk(node->children[i], enclosing, program, prog, reporter, had_error);
}

static void cc_process_fn(ASTNode *fnnode, ASTNode *program, Program *prog,
                          ErrorReporter *reporter, int *had_error) {
  FunctionDeclaration *fd = (FunctionDeclaration *)fnnode->data;
  if (!fd || !fd->body)
    return;
  CCEnv locals = {0};
  cc_collect_fn_env(fd, &locals);
  cc_walk(fd->body, &locals, program, prog, reporter, had_error);
  cc_env_free(&locals);
}

int closure_convert_program(ASTNode *program, ErrorReporter *reporter) {
  if (!program || program->type != AST_PROGRAM)
    return 1;
  Program *prog = (Program *)program->data;
  if (!prog)
    return 1;

  if (!g_closures_continue) {
    g_cc_counter = 0;
  }
  int had_error = 0;

  for (size_t i = 0; i < prog->declaration_count; i++) {
    ASTNode *decl = prog->declarations[i];
    if (!decl)
      continue;
    if (decl->type == AST_FUNCTION_DECLARATION ||
        decl->type == AST_METHOD_DECLARATION) {
      cc_process_fn(decl, program, prog, reporter, &had_error);
    } else if (decl->type == AST_STRUCT_DECLARATION) {
      StructDeclaration *sd = (StructDeclaration *)decl->data;
      for (size_t m = 0; sd && m < sd->method_count; m++)
        cc_process_fn(sd->methods[m], program, prog, reporter, &had_error);
    } else if (decl->type == AST_IMPL_DECLARATION) {
      ImplDeclaration *id = (ImplDeclaration *)decl->data;
      for (size_t m = 0; id && m < id->method_count; m++)
        cc_process_fn(id->methods[m], program, prog, reporter, &had_error);
    } else if (decl->type == AST_VAR_DECLARATION) {
      VarDeclaration *vd = (VarDeclaration *)decl->data;
      if (vd && vd->initializer) {
        CCEnv empty = {0};
        cc_walk(vd->initializer, &empty, program, prog, reporter, &had_error);
      }
    }
  }

  return had_error ? 0 : 1;
}

static void cc_detach_child(ASTNode *parent, ASTNode *child) {
  if (!parent || !child)
    return;
  for (size_t i = 0; i < parent->child_count; i++) {
    if (parent->children[i] == child) {
      for (size_t j = i + 1; j < parent->child_count; j++)
        parent->children[j - 1] = parent->children[j];
      parent->child_count--;
      return;
    }
  }
}

typedef struct {
  char **sigs;
  char **ctor_names;
  size_t count;
  size_t cap;
} AdaptCache;

static char *adapt_sig_key(char **param_types, size_t param_count,
                           const char *return_type) {
  CCBuf b = {0};
  for (size_t i = 0; i < param_count; i++) {
    if (i > 0)
      cc_buf_add(&b, ",");
    cc_buf_add(&b, param_types[i]);
  }
  cc_buf_add(&b, "->");
  cc_buf_add(&b, return_type ? return_type : "void");
  return b.data;
}

static const char *adapt_cache_find(const AdaptCache *c, const char *sig) {
  for (size_t i = 0; i < c->count; i++)
    if (strcmp(c->sigs[i], sig) == 0)
      return c->ctor_names[i];
  return NULL;
}

static void adapt_cache_add(AdaptCache *c, const char *sig,
                            const char *ctor_name) {
  if (c->count == c->cap) {
    size_t ncap = c->cap ? c->cap * 2 : 8;
    char **gs = realloc(c->sigs, ncap * sizeof(char *));
    char **gc = realloc(c->ctor_names, ncap * sizeof(char *));
    if (!gs || !gc) {
      free(gs);
      free(gc);
      return;
    }
    c->sigs = gs;
    c->ctor_names = gc;
    c->cap = ncap;
  }
  c->sigs[c->count] = strdup(sig);
  c->ctor_names[c->count] = strdup(ctor_name);
  c->count++;
}

static void adapt_cache_free(AdaptCache *c) {
  for (size_t i = 0; i < c->count; i++) {
    free(c->sigs[i]);
    free(c->ctor_names[i]);
  }
  free(c->sigs);
  free(c->ctor_names);
}

static int g_adapt_counter;

typedef struct {
  size_t *buckets;
  size_t bucket_count;
} AdaptFnIndex;

static AdaptFnIndex g_adapt_fn_index;

static void adapt_fn_index_build(ASTNode **top_decls, size_t top_count) {
  size_t nb = 64;
  while (nb < top_count * 2) {
    nb *= 2;
  }
  g_adapt_fn_index.buckets = calloc(nb, sizeof(size_t));
  g_adapt_fn_index.bucket_count = g_adapt_fn_index.buckets ? nb : 0;
  if (!g_adapt_fn_index.bucket_count) {
    return;
  }
  for (size_t i = 0; i < top_count; i++) {
    FunctionDeclaration *fd = (FunctionDeclaration *)top_decls[i]->data;
    if (!fd || !fd->name) {
      continue;
    }
    size_t b = mettle_fnv1a_hash(fd->name) & (nb - 1);
    while (g_adapt_fn_index.buckets[b]) {
      b = (b + 1) & (nb - 1);
    }
    g_adapt_fn_index.buckets[b] = i + 1;
  }
}

static void adapt_fn_index_free(void) {
  free(g_adapt_fn_index.buckets);
  g_adapt_fn_index.buckets = NULL;
  g_adapt_fn_index.bucket_count = 0;
}

static FunctionDeclaration *adapt_fn_index_find(ASTNode **top_decls,
                                                size_t top_count,
                                                const char *name,
                                                long long want_arity) {
  if (!g_adapt_fn_index.bucket_count) {
    for (size_t i = 0; i < top_count; i++) {
      FunctionDeclaration *fd = (FunctionDeclaration *)top_decls[i]->data;
      if (fd && fd->name && strcmp(fd->name, name) == 0 &&
          (want_arity < 0 || (long long)fd->parameter_count == want_arity)) {
        return fd;
      }
    }
    return NULL;
  }
  size_t nb = g_adapt_fn_index.bucket_count;
  size_t b = mettle_fnv1a_hash(name) & (nb - 1);
  while (g_adapt_fn_index.buckets[b]) {
    FunctionDeclaration *fd =
        (FunctionDeclaration *)top_decls[g_adapt_fn_index.buckets[b] - 1]->data;
    if (fd && fd->name && strcmp(fd->name, name) == 0 &&
        (want_arity < 0 || (long long)fd->parameter_count == want_arity)) {
      return fd;
    }
    b = (b + 1) & (nb - 1);
  }
  return NULL;
}

static const char *adapt_emit_adapter(char **param_types, size_t param_count,
                                      const char *return_type,
                                      ASTNode *program, Program *prog,
                                      int *had_error) {
  int id = g_adapt_counter++;
  char struct_name[32], thunk_name[32], make_name[32];
  snprintf(struct_name, sizeof(struct_name), "__Adapt_%d", id);
  snprintf(thunk_name, sizeof(thunk_name), "__athunk_%d", id);
  snprintf(make_name, sizeof(make_name), "__amake_%d", id);
  const char *ret = return_type ? return_type : "void";

  CCBuf src = {0};
  cc_buf_add(&src, "struct ");
  cc_buf_add(&src, struct_name);
  cc_buf_add(&src, " {\n  __code: fn(");
  cc_buf_add(&src, struct_name);
  cc_buf_add(&src, "*");
  for (size_t i = 0; i < param_count; i++) {
    cc_buf_add(&src, ", ");
    cc_buf_add(&src, param_types[i]);
  }
  cc_buf_add(&src, ") -> ");
  cc_buf_add(&src, ret);
  cc_buf_add(&src, ";\n  __real: fn(");
  for (size_t i = 0; i < param_count; i++) {
    if (i > 0)
      cc_buf_add(&src, ", ");
    cc_buf_add(&src, param_types[i]);
  }
  cc_buf_add(&src, ") -> ");
  cc_buf_add(&src, ret);
  cc_buf_add(&src, ";\n}\n");

  cc_buf_add(&src, "fn ");
  cc_buf_add(&src, thunk_name);
  cc_buf_add(&src, "(__env: ");
  cc_buf_add(&src, struct_name);
  cc_buf_add(&src, "*");
  for (size_t i = 0; i < param_count; i++) {
    cc_buf_add(&src, ", ");
    cc_buf_add_num(&src, "p", i);
    cc_buf_add(&src, ": ");
    cc_buf_add(&src, param_types[i]);
  }
  cc_buf_add(&src, ") -> ");
  cc_buf_add(&src, ret);
  cc_buf_add(&src, " {\n  ");
  cc_buf_add(&src, strcmp(ret, "void") == 0 ? "" : "return ");
  cc_buf_add(&src, "__env.__real(");
  for (size_t i = 0; i < param_count; i++) {
    if (i > 0)
      cc_buf_add(&src, ", ");
    cc_buf_add_num(&src, "p", i);
  }
  cc_buf_add(&src, ");\n}\n");

  cc_buf_add(&src, "fn ");
  cc_buf_add(&src, make_name);
  cc_buf_add(&src, "(__real: fn(");
  for (size_t i = 0; i < param_count; i++) {
    if (i > 0)
      cc_buf_add(&src, ", ");
    cc_buf_add(&src, param_types[i]);
  }
  cc_buf_add(&src, ") -> ");
  cc_buf_add(&src, ret);
  cc_buf_add(&src, ") -> ");
  cc_buf_add(&src, struct_name);
  cc_buf_add(&src, "* {\n  var __e: ");
  cc_buf_add(&src, struct_name);
  cc_buf_add(&src, "* = new ");
  cc_buf_add(&src, struct_name);
  cc_buf_add(&src, ";\n  __e.__code = &");
  cc_buf_add(&src, thunk_name);
  cc_buf_add(&src, ";\n  __e.__real = __real;\n  return __e;\n}\n");

  if (!src.data) {
    *had_error = 1;
    return NULL;
  }

  Lexer *lx = lexer_create(src.data);
  Parser *ps = lx ? parser_create(lx) : NULL;
  ASTNode *sub = ps ? parser_parse_program(ps) : NULL;
  Program *subp = (sub && sub->type == AST_PROGRAM) ? (Program *)sub->data : NULL;
  free(src.data);
  if (!subp || subp->declaration_count < 3) {
    *had_error = 1;
    return NULL;
  }

  for (size_t i = 0; i < subp->declaration_count; i++) {
    ASTNode *d = subp->declarations[i];
    ASTNode **grown =
        realloc(prog->declarations,
                (prog->declaration_count + 1) * sizeof(ASTNode *));
    if (!grown) {
      *had_error = 1;
      break;
    }
    prog->declarations = grown;
    prog->declarations[prog->declaration_count++] = d;
    ast_add_child(program, d);
  }

  return string_intern(make_name);
}

static const char *adapt_get_or_create(char **param_types, size_t param_count,
                                       const char *return_type,
                                       AdaptCache *cache, ASTNode *program,
                                       Program *prog, int *had_error) {
  char *sig = adapt_sig_key(param_types, param_count, return_type);
  if (!sig) {
    *had_error = 1;
    return NULL;
  }
  const char *existing = adapt_cache_find(cache, sig);
  if (existing) {
    free(sig);
    return existing;
  }
  const char *ctor =
      adapt_emit_adapter(param_types, param_count, return_type, program, prog,
                        had_error);
  if (ctor) {
    adapt_cache_add(cache, sig, ctor);
  }
  free(sig);
  return ctor;
}

static int adapt_type_is_closure_string(const char *s) {
  return s && strlen(s) >= 4 && strncmp(s, "Fn(", 3) == 0;
}

static int adapt_thin_signature(ASTNode *expr, ASTNode **top_decls,
                                size_t top_count, char ***out_param_types,
                                size_t *out_param_count,
                                char **out_return_type) {
  if (!expr)
    return 0;
  if (expr->type == AST_UNARY_EXPRESSION) {
    UnaryExpression *un = (UnaryExpression *)expr->data;
    if (!un || !un->operator || strcmp(un->operator, "&") != 0 || !un->operand ||
        un->operand->type != AST_IDENTIFIER) {
      return 0;
    }
    Identifier *id = (Identifier *)un->operand->data;
    if (!id || !id->name)
      return 0;
    FunctionDeclaration *fd =
        adapt_fn_index_find(top_decls, top_count, id->name, -1);
    if (fd) {
      *out_param_types = fd->parameter_types;
      *out_param_count = fd->parameter_count;
      *out_return_type = fd->return_type;
      return 1;
    }
    return 0;
  }
  if (expr->type == AST_LAMBDA_EXPRESSION) {
    FunctionDeclaration *fd = (FunctionDeclaration *)expr->data;
    if (!fd || fd->captured_count > 0)
      return 0;
    *out_param_types = fd->parameter_types;
    *out_param_count = fd->parameter_count;
    *out_return_type = fd->return_type;
    return 1;
  }
  return 0;
}

static void adapt_wrap_if_needed(ASTNode **slot, ASTNode *owner,
                                 const char *dest_type_str,
                                 ASTNode **top_decls, size_t top_count,
                                 AdaptCache *cache, ASTNode *program,
                                 Program *prog, int *had_error) {
  if (!slot || !*slot || !owner || !adapt_type_is_closure_string(dest_type_str))
    return;
  char **param_types = NULL;
  size_t param_count = 0;
  char *return_type = NULL;
  if (!adapt_thin_signature(*slot, top_decls, top_count, &param_types,
                            &param_count, &return_type)) {
    return;
  }
  const char *ctor = adapt_get_or_create(param_types, param_count, return_type,
                                        cache, program, prog, had_error);
  if (!ctor) {
    *had_error = 1;
    return;
  }
  ASTNode *inner = *slot;
  ASTNode *wrapper = ast_create_closure_adapt(inner, ctor, param_types,
                                              param_count, return_type,
                                              inner->location);
  if (!wrapper) {
    *had_error = 1;
    return;
  }
  cc_detach_child(owner, inner);
  ast_add_child(owner, wrapper);
  *slot = wrapper;
}

typedef struct {
  const char *name;
  const char *type_name;
} AdaptLocalType;

typedef struct {
  AdaptLocalType *items;
  size_t count;
  size_t capacity;
} AdaptLocals;

static void adapt_locals_collect(ASTNode *node, AdaptLocals *locals) {
  if (!node) {
    return;
  }
  if (node->type == AST_VAR_DECLARATION) {
    VarDeclaration *vd = (VarDeclaration *)node->data;
    if (vd && vd->name && vd->type_name) {
      if (locals->count == locals->capacity) {
        size_t grown = locals->capacity ? locals->capacity * 2 : 8;
        AdaptLocalType *items = (AdaptLocalType *)realloc(
            locals->items, grown * sizeof(AdaptLocalType));
        if (!items) {
          return;
        }
        locals->items = items;
        locals->capacity = grown;
      }
      locals->items[locals->count].name = vd->name;
      locals->items[locals->count].type_name = vd->type_name;
      locals->count++;
    }
  }
  for (size_t i = 0; i < node->child_count; i++) {
    adapt_locals_collect(node->children[i], locals);
  }
}

static const char *adapt_local_type(const AdaptLocals *locals,
                                    const char *name) {
  if (!locals || !name) {
    return NULL;
  }
  for (size_t i = locals->count; i-- > 0;) {
    if (locals->items[i].name && strcmp(locals->items[i].name, name) == 0) {
      return locals->items[i].type_name;
    }
  }
  return NULL;
}

static size_t mono_base_type_length(const char *type_str);

static const char *adapt_field_type(Program *prog, const char *struct_type,
                                    const char *field) {
  size_t base_length;
  if (!prog || !struct_type || !field) {
    return NULL;
  }
  base_length = mono_base_type_length(struct_type);
  for (size_t i = 0; i < prog->declaration_count; i++) {
    ASTNode *decl = prog->declarations[i];
    StructDeclaration *sd = NULL;
    if (!decl || decl->type != AST_STRUCT_DECLARATION) {
      continue;
    }
    sd = (StructDeclaration *)decl->data;
    if (!sd || !sd->name || strlen(sd->name) != base_length ||
        strncmp(sd->name, struct_type, base_length) != 0) {
      continue;
    }
    for (size_t f = 0; f < sd->field_count; f++) {
      if (sd->field_names[f] && strcmp(sd->field_names[f], field) == 0) {
        return sd->field_types[f];
      }
    }
    return NULL;
  }
  return NULL;
}

static const char *adapt_expression_type(ASTNode *node,
                                         const AdaptLocals *locals,
                                         Program *prog) {
  if (!node) {
    return NULL;
  }
  switch (node->type) {
  case AST_IDENTIFIER: {
    Identifier *id = (Identifier *)node->data;
    return id ? adapt_local_type(locals, id->name) : NULL;
  }
  case AST_INDEX_EXPRESSION: {
    ArrayIndexExpression *index = (ArrayIndexExpression *)node->data;
    return index ? adapt_expression_type(index->array, locals, prog) : NULL;
  }
  case AST_UNARY_EXPRESSION: {
    UnaryExpression *unary = (UnaryExpression *)node->data;
    if (unary && unary->operator&& strcmp(unary->operator, "*") == 0) {
      return adapt_expression_type(unary->operand, locals, prog);
    }
    return NULL;
  }
  case AST_MEMBER_ACCESS: {
    MemberAccess *member = (MemberAccess *)node->data;
    if (!member || !member->member) {
      return NULL;
    }
    return adapt_field_type(
        prog, adapt_expression_type(member->object, locals, prog),
        member->member);
  }
  default:
    return NULL;
  }
}

static const char *adapt_assignment_target_type(const Assignment *assign,
                                                const AdaptLocals *locals,
                                                Program *prog) {
  if (!assign) {
    return NULL;
  }
  if (!assign->target) {
    return adapt_local_type(locals, assign->variable_name);
  }
  return adapt_expression_type(assign->target, locals, prog);
}

static void adapt_walk(ASTNode *node, const char *current_return_type,
                       ASTNode **top_decls, size_t top_count,
                       AdaptCache *cache, ASTNode *program, Program *prog,
                       int *had_error, const AdaptLocals *locals) {
  if (!node || node->type == AST_LAMBDA_EXPRESSION ||
      node->type == AST_CLOSURE_ADAPT_EXPRESSION)
    return;

  if (node->type == AST_VAR_DECLARATION) {
    VarDeclaration *vd = (VarDeclaration *)node->data;
    if (vd && vd->initializer) {
      adapt_wrap_if_needed(&vd->initializer, node, vd->type_name, top_decls,
                           top_count, cache, program, prog, had_error);
    }
  } else if (node->type == AST_RETURN_STATEMENT) {
    ReturnStatement *rs = (ReturnStatement *)node->data;
    if (rs && rs->value) {
      adapt_wrap_if_needed(&rs->value, node, current_return_type, top_decls,
                           top_count, cache, program, prog, had_error);
      if (rs->values && rs->value_count > 0) {
        rs->values[0] = rs->value;
      }
    }
  } else if (node->type == AST_ASSIGNMENT) {
    Assignment *assign = (Assignment *)node->data;
    if (assign && assign->value) {
      adapt_wrap_if_needed(
          &assign->value, node,
          adapt_assignment_target_type(assign, locals, prog), top_decls,
          top_count, cache, program, prog, had_error);
    }
  } else if (node->type == AST_FUNCTION_CALL) {
    CallExpression *call = (CallExpression *)node->data;
    if (call && !call->object && call->function_name) {
      FunctionDeclaration *fd =
          adapt_fn_index_find(top_decls, top_count, call->function_name,
                              (long long)call->argument_count);
      if (fd) {
        for (size_t a = 0; a < call->argument_count; a++) {
          adapt_wrap_if_needed(&call->arguments[a], node,
                               fd->parameter_types[a], top_decls, top_count,
                               cache, program, prog, had_error);
        }
      }
    }
  }

  for (size_t i = 0; i < node->child_count; i++)
    adapt_walk(node->children[i], current_return_type, top_decls, top_count,
              cache, program, prog, had_error, locals);
}

static void adapt_process_fn(ASTNode *fnnode, ASTNode **top_decls,
                             size_t top_count, AdaptCache *cache,
                             ASTNode *program, Program *prog, int *had_error) {
  FunctionDeclaration *fd = (FunctionDeclaration *)fnnode->data;
  AdaptLocals locals = {0};
  if (!fd || !fd->body)
    return;
  adapt_locals_collect(fd->body, &locals);
  adapt_walk(fd->body, fd->return_type, top_decls, top_count, cache, program,
            prog, had_error, &locals);
  free(locals.items);
}

int closure_adapt_program(ASTNode *program, ErrorReporter *reporter) {
  (void)reporter;
  if (!program || program->type != AST_PROGRAM)
    return 1;
  Program *prog = (Program *)program->data;
  if (!prog)
    return 1;

  if (!g_closures_continue) {
    g_adapt_counter = 0;
  }
  int had_error = 0;

  size_t whole_count = g_adapt_whole ? g_adapt_whole->declaration_count : 0;
  ASTNode **top_decls =
      malloc((prog->declaration_count + whole_count) * sizeof(ASTNode *));
  size_t top_count = 0;
  if (!top_decls && prog->declaration_count + whole_count > 0) {
    return 0;
  }
  for (size_t i = 0; i < prog->declaration_count; i++) {
    if (prog->declarations[i] &&
        prog->declarations[i]->type == AST_FUNCTION_DECLARATION) {
      top_decls[top_count++] = prog->declarations[i];
    }
  }
  for (size_t i = 0; i < whole_count; i++) {
    if (g_adapt_whole->declarations[i] &&
        g_adapt_whole->declarations[i]->type == AST_FUNCTION_DECLARATION) {
      top_decls[top_count++] = g_adapt_whole->declarations[i];
    }
  }

  AdaptCache cache = {0};
  adapt_fn_index_build(top_decls, top_count);

  for (size_t i = 0; i < prog->declaration_count; i++) {
    ASTNode *decl = prog->declarations[i];
    if (!decl)
      continue;
    if (decl->type == AST_FUNCTION_DECLARATION ||
        decl->type == AST_METHOD_DECLARATION) {
      adapt_process_fn(decl, top_decls, top_count, &cache, program, prog,
                       &had_error);
    } else if (decl->type == AST_STRUCT_DECLARATION) {
      StructDeclaration *sd = (StructDeclaration *)decl->data;
      for (size_t m = 0; sd && m < sd->method_count; m++)
        adapt_process_fn(sd->methods[m], top_decls, top_count, &cache,
                         program, prog, &had_error);
    } else if (decl->type == AST_IMPL_DECLARATION) {
      ImplDeclaration *id = (ImplDeclaration *)decl->data;
      for (size_t m = 0; id && m < id->method_count; m++)
        adapt_process_fn(id->methods[m], top_decls, top_count, &cache,
                         program, prog, &had_error);
    } else if (decl->type == AST_VAR_DECLARATION) {
      VarDeclaration *vd = (VarDeclaration *)decl->data;
      if (vd && vd->initializer) {
        adapt_wrap_if_needed(&vd->initializer, decl, vd->type_name, top_decls,
                             top_count, &cache, program, prog, &had_error);
      }
    }
  }

  adapt_fn_index_free();
  free(top_decls);
  adapt_cache_free(&cache);
  return had_error ? 0 : 1;
}

static size_t mono_base_type_length(const char *type_str) {
  size_t length = strlen(type_str);
  const char *bracket = strchr(type_str, '[');

  if (bracket) {
    length = (size_t)(bracket - type_str);
  }
  while (length > 0 && type_str[length - 1] == '*') {
    length--;
  }
  return length;
}

static int mono_name_is(const char *name, const char *type_str,
                        size_t base_length) {
  return name && strlen(name) == base_length &&
         strncmp(name, type_str, base_length) == 0;
}

static size_t mono_find_struct_index(Program *prog, const char *type_str,
                                     size_t base_length) {
  for (size_t i = 0; i < prog->declaration_count; i++) {
    ASTNode *decl = prog->declarations[i];
    StructDeclaration *sd = NULL;
    if (!decl || decl->type != AST_STRUCT_DECLARATION) {
      continue;
    }
    sd = (StructDeclaration *)decl->data;
    if (sd && mono_name_is(sd->name, type_str, base_length)) {
      return i;
    }
  }
  return (size_t)-1;
}

static int mono_is_instantiated_struct(MonoContext *ctx, const char *type_str,
                                       size_t base_length) {
  for (size_t i = 0; i < ctx->instance_count; i++) {
    if (ctx->instances[i].emitted &&
        mono_name_is(ctx->instances[i].mangled_name, type_str, base_length)) {
      GenericDef *def = find_generic_def(ctx, ctx->instances[i].generic_name);
      return def && def->is_struct;
    }
  }
  return 0;
}

static void mono_move_declaration(Program *prog, size_t from, size_t to) {
  ASTNode *node = prog->declarations[from];

  if (from <= to) {
    return;
  }
  memmove(&prog->declarations[to + 1], &prog->declarations[to],
          (from - to) * sizeof(ASTNode *));
  prog->declarations[to] = node;
}

static void mono_order_instantiations_before_use(MonoContext *ctx,
                                                 Program *prog) {
  size_t index = 0;
  size_t budget = 0;

  if (!ctx || !prog || ctx->instance_count == 0) {
    return;
  }
  budget = ctx->instance_count;

  while (index < prog->declaration_count) {
    ASTNode *decl = prog->declarations[index];
    StructDeclaration *sd = NULL;
    int moved = 0;

    if (decl && decl->type == AST_STRUCT_DECLARATION) {
      sd = (StructDeclaration *)decl->data;
    }
    for (size_t f = 0; sd && f < sd->field_count; f++) {
      const char *field_type = sd->field_types[f];
      size_t base_length = field_type ? mono_base_type_length(field_type) : 0;
      size_t declared_at = 0;

      if (base_length == 0 ||
          !mono_is_instantiated_struct(ctx, field_type, base_length)) {
        continue;
      }
      declared_at = mono_find_struct_index(prog, field_type, base_length);
      if (declared_at != (size_t)-1 && declared_at > index && budget > 0) {
        mono_move_declaration(prog, declared_at, index);
        budget--;
        moved = 1;
        break;
      }
    }

    if (!moved) {
      index++;
    }
  }
}

static ASTNode **g_mono_retained = NULL;
static size_t g_mono_retained_count = 0;
static Instantiation *g_mono_pending = NULL;
static size_t g_mono_pending_count = 0;

static void mono_keep_pending(const Instantiation *instance) {
  Instantiation *grown = realloc(
      g_mono_pending, (g_mono_pending_count + 1) * sizeof(Instantiation));
  if (!grown) {
    return;
  }
  g_mono_pending = grown;
  Instantiation *kept = &g_mono_pending[g_mono_pending_count++];
  memset(kept, 0, sizeof(*kept));
  kept->generic_name = strdup(instance->generic_name);
  kept->type_arg_count = instance->type_arg_count;
  kept->location = instance->location;
  kept->type_args = calloc(instance->type_arg_count + 1, sizeof(char *));
  for (size_t i = 0; kept->type_args && i < instance->type_arg_count; i++) {
    kept->type_args[i] = strdup(instance->type_args[i]);
  }
}

static void mono_retain(ASTNode *decl) {
  for (size_t i = 0; i < g_mono_retained_count; i++) {
    if (g_mono_retained[i] == decl) {
      return;
    }
  }
  ASTNode **grown = realloc(g_mono_retained,
                            (g_mono_retained_count + 1) * sizeof(ASTNode *));
  if (grown) {
    g_mono_retained = grown;
    g_mono_retained[g_mono_retained_count++] = decl;
  }
}

static const char *mono_decl_name(const ASTNode *decl) {
  if (decl->type == AST_STRUCT_DECLARATION && decl->data) {
    return ((const StructDeclaration *)decl->data)->name;
  }
  if (decl->type == AST_FUNCTION_DECLARATION && decl->data) {
    return ((const FunctionDeclaration *)decl->data)->name;
  }
  return NULL;
}

static int mono_program_declares(const Program *program, const char *name) {
  for (size_t i = 0; i < program->declaration_count; i++) {
    const ASTNode *decl = program->declarations[i];
    const char *held = decl ? mono_decl_name(decl) : NULL;
    if (held && strcmp(held, name) == 0) {
      return 1;
    }
  }
  return 0;
}

int monomorphize_revisit(ASTNode *whole, ErrorReporter *reporter) {
  Program *prog = (Program *)whole->data;
  MonoContext ctx = {0};
  int success = 1;
  ASTNode *holder = ast_create_node(AST_PROGRAM, whole->location);
  Program *held = calloc(1, sizeof(Program));
  if (!holder || !held) {
    return 0;
  }
  held->declarations = g_mono_retained;
  held->declaration_count = g_mono_retained_count;
  holder->data = held;
  ctx.reporter = reporter;
  collect_generic_defs(holder, &ctx);
  if (ctx.had_error || ctx.def_count == 0) {
    return !ctx.had_error;
  }
  ctx.module = prog;
  if (!mono_infer_type_args(&ctx, whole)) {
    return 0;
  }
  collect_type_instantiations(whole, &ctx);
  for (size_t i = 0; i < g_mono_pending_count; i++) {
    mono_add_instantiation(&ctx, g_mono_pending[i].generic_name,
                           g_mono_pending[i].type_args,
                           g_mono_pending[i].type_arg_count,
                           g_mono_pending[i].location);
  }
  size_t processed = 0;
  while (processed < ctx.instance_count) {
    size_t current = ctx.instance_count;
    for (size_t i = processed; i < current; i++) {
      if (!mono_emit_instantiation(&ctx, prog, whole, i, &success) || !success) {
        return 0;
      }
    }
    processed = current;
  }
  rewrite_generic_references(whole, &ctx);
  mono_order_instantiations_before_use(&ctx, prog);
  size_t kept = 0;
  for (size_t i = 0; i < prog->declaration_count; i++) {
    ASTNode *decl = prog->declarations[i];
    const char *name = decl ? mono_decl_name(decl) : NULL;
    int duplicate = 0;
    for (size_t k = 0; name && k < ctx.instance_count && !duplicate; k++) {
      if (ctx.instances[k].mangled_name &&
          strcmp(ctx.instances[k].mangled_name, name) == 0) {
        for (size_t earlier = 0; earlier < kept && !duplicate; earlier++) {
          const char *other = mono_decl_name(prog->declarations[earlier]);
          duplicate = other && strcmp(other, name) == 0;
        }
      }
    }
    if (!duplicate) {
      prog->declarations[kept++] = decl;
    }
  }
  prog->declaration_count = kept;
  free(whole->children);
  whole->children = NULL;
  whole->child_count = 0;
  whole->child_capacity = 0;
  whole->child_capacity_owner = NULL;
  for (size_t i = 0; i < kept; i++) {
    ast_add_child(whole, prog->declarations[i]);
  }
  free(ctx.decl_slots);
  return !ctx.had_error;
}

int monomorphize_generated(ASTNode *fragment, ASTNode *whole,
                           ErrorReporter *reporter, int *new_generics) {
  Program *prog = (Program *)fragment->data;
  Program *whole_prog = (Program *)whole->data;
  size_t own_count = prog->declaration_count;
  size_t retained_count = g_mono_retained_count;
  size_t kept = 0;
  int ok = 1;
  ASTNode **own = malloc((own_count + 1) * sizeof(ASTNode *));
  ASTNode **joined =
      malloc((own_count + retained_count + 1) * sizeof(ASTNode *));
  if (!own || !joined) {
    return 0;
  }
  memcpy(own, prog->declarations, own_count * sizeof(ASTNode *));
  memcpy(joined, g_mono_retained, retained_count * sizeof(ASTNode *));
  memcpy(joined + retained_count, own, own_count * sizeof(ASTNode *));
  free(prog->declarations);
  prog->declarations = joined;
  prog->declaration_count = own_count + retained_count;
  ok = monomorphize_program(fragment, reporter);
  *new_generics = g_mono_retained_count > retained_count;
  if (ok) {
    for (size_t i = 0; i < prog->declaration_count; i++) {
      ASTNode *decl = prog->declarations[i];
      int was_own = 0;
      int was_retained = 0;
      const char *name = decl ? mono_decl_name(decl) : NULL;
      for (size_t k = 0; k < own_count && !was_own; k++) {
        was_own = own[k] == decl;
      }
      for (size_t k = 0; k < g_mono_retained_count && !was_retained; k++) {
        was_retained = g_mono_retained[k] == decl;
      }
      if (was_retained ||
          (!was_own && name && mono_program_declares(whole_prog, name))) {
        continue;
      }
      prog->declarations[kept++] = decl;
    }
    prog->declaration_count = kept;
    free(fragment->children);
    fragment->children = NULL;
    fragment->child_count = 0;
    fragment->child_capacity = 0;
    fragment->child_capacity_owner = NULL;
    for (size_t i = 0; i < kept; i++) {
      ast_add_child(fragment, prog->declarations[i]);
    }
    g_closures_continue = 1;
    g_adapt_whole = whole_prog;
    ok = closure_convert_program(fragment, reporter) &&
         closure_adapt_program(fragment, reporter);
    g_closures_continue = 0;
    g_adapt_whole = NULL;
  }
  free(own);
  return ok;
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

  collect_generic_defs(program, &ctx);
  if (ctx.had_error) {
    success = 0;
    goto cleanup;
  }

  if (!mono_emit_impl_method_functions(&ctx, program)) {
    success = 0;
    goto cleanup;
  }

  ctx.module = prog;
  if (!mono_infer_type_args(&ctx, program)) {
    success = 0;
    goto cleanup;
  }

  if (ctx.def_count > 0) {
    collect_type_instantiations(program, &ctx);

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

    rewrite_generic_references(program, &ctx);

    mono_order_instantiations_before_use(&ctx, prog);
  }

  if (!mono_rewrite_trait_method_calls(&ctx, program)) {
    success = 0;
    goto cleanup;
  }

  if (!mono_lift_struct_methods(&ctx, prog, program)) {
    success = 0;
    goto cleanup;
  }

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
      if (decl->type == AST_TRAIT_DECLARATION && decl->data &&
          ((TraitDeclaration *)decl->data)->meta_decorator_count > 0) {
        TraitDeclaration *td = (TraitDeclaration *)decl->data;
        is_compile_time_trait_decl = 0;
        td->implementors = calloc(ctx.impl_count + 1, sizeof(char *));
        td->implementor_count = 0;
        for (size_t k = 0; td->implementors && k < ctx.impl_count; k++) {
          if (ctx.impls[k].trait_name &&
              strcmp(ctx.impls[k].trait_name, td->name) == 0) {
            td->implementors[td->implementor_count++] =
                strdup(ctx.impls[k].for_type_name);
          }
        }
      }
    }

    if (is_generic || is_compile_time_trait_decl ||
        (decl && decl->type == AST_TRAIT_DECLARATION)) {
      mono_retain(decl);
    }
    if (!is_generic && !is_compile_time_trait_decl) {
      prog->declarations[write_idx++] = decl;
    }
  }
  prog->declaration_count = write_idx;

cleanup:
  free(ctx.decl_slots);
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
    if (!ctx.instances[i].emitted) {
      mono_keep_pending(&ctx.instances[i]);
    }
    free(ctx.instances[i].generic_name);
    free(ctx.instances[i].mangled_name);
    for (size_t j = 0; j < ctx.instances[i].type_arg_count; j++)
      free(ctx.instances[i].type_args[j]);
    free(ctx.instances[i].type_args);
  }
  free(ctx.instances);

  return success && !ctx.had_error;
}
