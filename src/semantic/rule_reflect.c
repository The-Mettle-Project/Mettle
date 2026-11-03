#include "rule_reflect.h"
#include "../codegen/gpu_structure.h"
#include "../ir/ir_machine.h"
#include "../ir/ir_trace.h"
#include "import_resolver.h"
#include "type_checker_internal.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  const char *name;
  char *qualified;
  const char *module;
  IRRuleSite site;
  const char **callees;
  size_t callee_count;
  size_t callee_capacity;
  const char **matches;
  size_t match_count;
  size_t match_capacity;
  const char **param_types;
  size_t param_count;
  const char *return_type;
  int is_extern;
  int is_exported;
  int is_recursive;
  int is_address_taken;
  int has_indirect_calls;
  int is_noalloc;
  int is_pure;
  int is_inline;
  int is_swappable;
  int is_kernel;
  const char *declared_name;
  const char **effects;
  size_t effect_count;
  const char **requires;
  size_t require_count;
  const char **forbids;
  size_t forbid_count;
  const char **provides;
  size_t provide_count;
  IRRuleSite *allocations;
  size_t allocation_count;
  size_t allocation_capacity;
  IRRuleSite *frees;
  size_t free_count;
  size_t free_capacity;
  const char **writes_globals;
  size_t write_count;
  size_t write_capacity;
  int returns_pointer;
  int scc_index;
  int scc_lowlink;
  int scc_on_stack;
} RFunction;

typedef struct {
  const char *name;
  const char *module;
  IRRuleSite site;
  const char *type_name;
  const char **written_by;
  size_t written_count;
  size_t written_capacity;
  int address_taken;
} RGlobal;

typedef struct {
  const char *name;
  char *qualified;
  const char *module;
  IRRuleSite site;
  const char *kind;
  const char *base;
  long long size;
  long long align;
  long long layout;
  const Type *type;
} RType;

typedef struct {
  unsigned char *bytes;
  size_t size;
  size_t capacity;
  size_t *pointers;
  size_t pointer_count;
  size_t pointer_capacity;
  unsigned char *pool;
  size_t pool_size;
  size_t pool_capacity;
  size_t *string_fields;
  size_t *string_offsets;
  size_t string_count;
  size_t string_capacity;
  int failed;
} Image;

typedef struct {
  TypeChecker *checker;
  RFunction *functions;
  size_t function_count;
  size_t function_capacity;
  RType *types;
  size_t type_count;
  size_t type_capacity;
  const char **modules;
  size_t module_count;
  size_t module_capacity;
  char **owned;
  size_t owned_count;
  size_t owned_capacity;
  RGlobal *globals;
  size_t global_count;
  size_t global_capacity;
  RFunction *current;
} Reflect;

static const char *strip_import_prefix(const char *name) {
  const char *p = name ? name : "";
  while (strncmp(p, "__import_", 9) == 0) {
    const char *rest = p + 9;
    while (*rest && *rest != '_') {
      rest++;
    }
    if (*rest != '_') {
      break;
    }
    p = rest + 1;
  }
  return p;
}

const char *rule_reflect_strip_import_prefix(const char *name) {
  return strip_import_prefix(name);
}

static char *qualify(const char *module, const char *name) {
  size_t length = strlen(module) + strlen(name) + 2;
  char *out = malloc(length);
  if (!out) {
    return NULL;
  }
  if (module[0]) {
    snprintf(out, length, "%s.%s", module, name);
  } else {
    snprintf(out, length, "%s", name);
  }
  return out;
}

static const char *module_of(const char *filename) {
  const char *module = filename ? import_resolver_module_for_file(filename)
                                : NULL;
  return module ? module : "";
}

static int grow(void **items, size_t *capacity, size_t count,
                size_t item_size) {
  if (count < *capacity) {
    return 1;
  }
  size_t next = *capacity ? *capacity * 2 : 16;
  void *grown = realloc(*items, next * item_size);
  if (!grown) {
    return 0;
  }
  *items = grown;
  *capacity = next;
  return 1;
}

static int note_module(Reflect *reflect, const char *module) {
  if (!module || !module[0]) {
    return 1;
  }
  for (size_t i = 0; i < reflect->module_count; i++) {
    if (strcmp(reflect->modules[i], module) == 0) {
      return 1;
    }
  }
  if (!grow((void **)&reflect->modules, &reflect->module_capacity,
            reflect->module_count, sizeof(const char *))) {
    return 0;
  }
  reflect->modules[reflect->module_count++] = module;
  return 1;
}

static RFunction *find_function(Reflect *reflect, const char *declared_name) {
  if (!declared_name) {
    return NULL;
  }
  for (size_t i = 0; i < reflect->function_count; i++) {
    if (strcmp(reflect->functions[i].declared_name, declared_name) == 0) {
      return &reflect->functions[i];
    }
  }
  return NULL;
}

static int add_unique(const char ***items, size_t *count, size_t *capacity,
                      const char *text) {
  for (size_t i = 0; i < *count; i++) {
    if (strcmp((*items)[i], text) == 0) {
      return 1;
    }
  }
  if (!grow((void **)items, capacity, *count, sizeof(const char *))) {
    return 0;
  }
  (*items)[(*count)++] = text;
  return 1;
}

static int add_callee(RFunction *function, const char *qualified) {
  return add_unique(&function->callees, &function->callee_count,
                    &function->callee_capacity, qualified);
}

static const char *own_string(Reflect *reflect, char *text) {
  if (!text) {
    return NULL;
  }
  if (!grow((void **)&reflect->owned, &reflect->owned_capacity,
            reflect->owned_count, sizeof(char *))) {
    free(text);
    return NULL;
  }
  reflect->owned[reflect->owned_count++] = text;
  return text;
}

static const char *type_display_name(const Type *type);

static int add_match(Reflect *reflect, RFunction *function, const Type *owner,
                     const char *variant) {
  if (!owner || !variant) {
    return 1;
  }
  const char *shown = strip_import_prefix(variant);
  const char *owner_name = type_display_name(owner);
  size_t length = strlen(owner_name) + strlen(shown) + 2;
  char *text = malloc(length);
  if (!text) {
    return 0;
  }
  snprintf(text, length, "%s.%s", owner_name, shown);
  for (size_t i = 0; i < function->match_count; i++) {
    if (strcmp(function->matches[i], text) == 0) {
      free(text);
      return 1;
    }
  }
  const char *kept = own_string(reflect, text);
  return kept && add_unique(&function->matches, &function->match_count,
                            &function->match_capacity, kept);
}

static int add_site_fact(IRRuleSite **sites, size_t *count, size_t *capacity,
                         SourceLocation location) {
  IRRuleSite site;
  if (!grow((void **)sites, capacity, *count, sizeof(IRRuleSite))) {
    return 0;
  }
  site.file = location.filename;
  site.line = location.line;
  site.column = location.column;
  (*sites)[(*count)++] = site;
  return 1;
}

static RGlobal *find_global(Reflect *reflect, const char *name) {
  if (!name) {
    return NULL;
  }
  for (size_t i = 0; i < reflect->global_count; i++) {
    if (strcmp(reflect->globals[i].name, name) == 0) {
      return &reflect->globals[i];
    }
  }
  return NULL;
}

static int note_global_write(Reflect *reflect, RFunction *function,
                             const char *name) {
  RGlobal *global = find_global(reflect, name);
  if (!global) {
    return 1;
  }
  if (!add_unique(&function->writes_globals, &function->write_count,
                  &function->write_capacity, global->name)) {
    return 0;
  }
  return add_unique(&global->written_by, &global->written_count,
                    &global->written_capacity, function->qualified);
}

static int allocator_name(const char *name) {
  static const char *const names[] = {"malloc",  "calloc",  "realloc",
                                      "strdup",  "new",     NULL};
  const char *shown = strip_import_prefix(name ? name : "");
  for (size_t i = 0; names[i]; i++) {
    if (strcmp(shown, names[i]) == 0) {
      return 1;
    }
  }
  return 0;
}

static int walk_body(Reflect *reflect, ASTNode *node) {
  if (!node) {
    return 1;
  }
  RFunction *current = reflect->current;
  if (node->type == AST_NEW_EXPRESSION) {
    if (!add_site_fact(&current->allocations, &current->allocation_count,
                       &current->allocation_capacity, node->location)) {
      return 0;
    }
  } else if (node->type == AST_ASSIGNMENT && node->data) {
    const Assignment *assign = (const Assignment *)node->data;
    if (assign->variable_name &&
        !note_global_write(reflect, current, assign->variable_name)) {
      return 0;
    }
  }
  if (node->type == AST_FUNCTION_CALL && node->data) {
    CallExpression *call = (CallExpression *)node->data;
    if (call->is_indirect_call) {
      current->has_indirect_calls = 1;
    } else if (call->function_name) {
      const char *shown = strip_import_prefix(call->function_name);
      if (strcmp(shown, "free") == 0) {
        if (!add_site_fact(&current->frees, &current->free_count,
                           &current->free_capacity, node->location)) {
          return 0;
        }
      } else if (allocator_name(call->function_name)) {
        if (!add_site_fact(&current->allocations, &current->allocation_count,
                           &current->allocation_capacity, node->location)) {
          return 0;
        }
      }
      RFunction *callee = find_function(reflect, call->function_name);
      const char *spelling =
          callee ? callee->qualified : strip_import_prefix(call->function_name);
      if (strncmp(spelling, "__", 2) != 0 && !add_callee(current, spelling)) {
        return 0;
      }
    }
  } else if (node->type == AST_FUNC_PTR_CALL) {
    current->has_indirect_calls = 1;
  } else if (node->type == AST_MATCH_STATEMENT && node->data) {
    MatchStatement *match = (MatchStatement *)node->data;
    const Type *owner = match->expression ? match->expression->resolved_type
                                          : NULL;
    if (owner && owner->kind == TYPE_POINTER && owner->base_type) {
      owner = owner->base_type;
    }
    for (size_t i = 0; i < match->arm_count; i++) {
      if (!match->arms[i].is_default &&
          !add_match(reflect, current, owner, match->arms[i].variant_name)) {
        return 0;
      }
    }
  } else if (node->type == AST_SWITCH_STATEMENT && node->data) {
    SwitchStatement *sw = (SwitchStatement *)node->data;
    for (size_t i = 0; i < sw->case_count; i++) {
      CaseClause *clause =
          sw->cases[i] && sw->cases[i]->data ? (CaseClause *)sw->cases[i]->data
                                             : NULL;
      if (!clause || clause->is_default || !clause->value ||
          clause->value->type != AST_MEMBER_ACCESS || !clause->value->data) {
        continue;
      }
      MemberAccess *member = (MemberAccess *)clause->value->data;
      const Type *owner = clause->value->resolved_type;
      if (owner && owner->kind == TYPE_ENUM &&
          !add_match(reflect, current, owner, member->member)) {
        return 0;
      }
    }
  } else if (node->type == AST_IDENTIFIER && node->data) {
    Identifier *identifier = (Identifier *)node->data;
    RFunction *named = find_function(reflect, identifier->name);
    if (named) {
      named->is_address_taken = 1;
    }
  }
  for (size_t i = 0; i < node->child_count; i++) {
    if (!walk_body(reflect, node->children[i])) {
      return 0;
    }
  }
  return 1;
}

static int collect_global(Reflect *reflect, ASTNode *decl) {
  const VarDeclaration *var = (const VarDeclaration *)decl->data;
  RGlobal *entry;
  if (!var || !var->name || var->is_const) {
    return 1;
  }
  if (!grow((void **)&reflect->globals, &reflect->global_capacity,
            reflect->global_count, sizeof(RGlobal))) {
    return 0;
  }
  entry = &reflect->globals[reflect->global_count++];
  memset(entry, 0, sizeof(*entry));
  entry->name = var->name;
  entry->module = module_of(decl->location.filename);
  entry->site.file = decl->location.filename;
  entry->site.line = decl->location.line;
  entry->site.column = decl->location.column;
  entry->type_name = var->type_name ? var->type_name : "";
  return note_module(reflect, entry->module);
}

static int collect_function(Reflect *reflect, ASTNode *decl) {
  FunctionDeclaration *fd = (FunctionDeclaration *)decl->data;
  if (!fd || !fd->name || fd->is_test || fd->is_rule || fd->rewrite_role ||
      fd->meta_role) {
    return 1;
  }
  if (!grow((void **)&reflect->functions, &reflect->function_capacity,
            reflect->function_count, sizeof(RFunction))) {
    return 0;
  }
  RFunction *function = &reflect->functions[reflect->function_count++];
  memset(function, 0, sizeof(*function));
  function->declared_name = fd->name;
  function->name = strip_import_prefix(fd->name);
  function->module = module_of(decl->location.filename);
  function->qualified = qualify(function->module, function->name);
  if (!function->qualified || !note_module(reflect, function->module)) {
    return 0;
  }
  function->site.file = decl->location.filename;
  function->site.line = decl->location.line;
  function->site.column = decl->location.column;
  function->param_types = (const char **)fd->parameter_types;
  function->param_count = fd->parameter_count;
  function->return_type = fd->return_type ? fd->return_type : "void";
  function->is_extern = fd->is_extern;
  function->is_exported = fd->is_exported;
  function->is_noalloc = fd->is_noalloc;
  function->is_pure = fd->is_pure;
  function->is_inline = fd->is_inline;
  function->is_swappable = fd->is_swappable;
  function->is_kernel = fd->is_kernel;
  function->forbids = (const char **)fd->effects_forbids;
  function->forbid_count = fd->effects_forbids_count;
  function->provides = (const char **)fd->effects_provides;
  function->provide_count = fd->effects_provides_count;
  function->effects = (const char **)fd->effects_with;
  function->effect_count = fd->effects_with_count;
  function->requires = (const char **)fd->effects_requires;
  function->require_count = fd->effects_requires_count;
  return 1;
}

static int collect_type(Reflect *reflect, ASTNode *decl) {
  const char *name = NULL;
  size_t type_params = 0;
  if (decl->type == AST_STRUCT_DECLARATION && decl->data) {
    StructDeclaration *sd = (StructDeclaration *)decl->data;
    name = sd->name;
    type_params = sd->type_param_count;
  } else if (decl->type == AST_ENUM_DECLARATION && decl->data) {
    EnumDeclaration *ed = (EnumDeclaration *)decl->data;
    name = ed->name;
    type_params = ed->type_param_count;
  } else if (decl->type == AST_TYPE_DECLARATION && decl->data) {
    name = ((TypeDeclaration *)decl->data)->name;
  }
  if (!name || type_params > 0) {
    return 1;
  }
  Type *type = type_checker_get_type_by_name(reflect->checker, name);
  if (!type) {
    return 1;
  }
  if (!type->refined_base && type->kind != TYPE_STRUCT &&
      type->kind != TYPE_ENUM && type->kind != TYPE_TAGGED_ENUM) {
    return 1;
  }
  if (!grow((void **)&reflect->types, &reflect->type_capacity,
            reflect->type_count, sizeof(RType))) {
    return 0;
  }
  RType *entry = &reflect->types[reflect->type_count++];
  memset(entry, 0, sizeof(*entry));
  entry->name = strip_import_prefix(name);
  entry->module = module_of(decl->location.filename);
  entry->qualified = qualify(entry->module, entry->name);
  if (!entry->qualified || !note_module(reflect, entry->module)) {
    return 0;
  }
  entry->site.file = decl->location.filename;
  entry->site.line = decl->location.line;
  entry->site.column = decl->location.column;
  entry->kind = type->refined_base            ? "declared"
                : type->kind == TYPE_STRUCT   ? "struct"
                : type->kind == TYPE_ENUM     ? "enum"
                                              : "tagged_enum";
  entry->base = type->refined_base ? type_display_name(type->refined_base) : "";
  entry->size = (long long)type->size;
  entry->align = (long long)type->alignment;
  entry->layout = type_checker_layout_digest(type);
  entry->type = type;
  return 1;
}

typedef struct {
  int *stack;
  size_t depth;
  int next_index;
} Tarjan;

static size_t function_index(Reflect *reflect, RFunction *function) {
  return (size_t)(function - reflect->functions);
}

static RFunction *find_by_qualified(Reflect *reflect, const char *qualified) {
  for (size_t i = 0; i < reflect->function_count; i++) {
    if (strcmp(reflect->functions[i].qualified, qualified) == 0) {
      return &reflect->functions[i];
    }
  }
  return NULL;
}

static void tarjan_visit(Reflect *reflect, Tarjan *state, RFunction *v) {
  v->scc_index = state->next_index;
  v->scc_lowlink = state->next_index;
  state->next_index++;
  state->stack[state->depth++] = (int)function_index(reflect, v);
  v->scc_on_stack = 1;
  for (size_t i = 0; i < v->callee_count; i++) {
    RFunction *w = find_by_qualified(reflect, v->callees[i]);
    if (!w) {
      continue;
    }
    if (w->scc_index < 0) {
      tarjan_visit(reflect, state, w);
      if (w->scc_lowlink < v->scc_lowlink) {
        v->scc_lowlink = w->scc_lowlink;
      }
    } else if (w->scc_on_stack && w->scc_index < v->scc_lowlink) {
      v->scc_lowlink = w->scc_index;
    }
  }
  if (v->scc_lowlink == v->scc_index) {
    size_t members = 0;
    size_t first = state->depth;
    while (first > 0) {
      first--;
      members++;
      if (state->stack[first] == (int)function_index(reflect, v)) {
        break;
      }
    }
    int self_edge = 0;
    for (size_t i = 0; i < v->callee_count; i++) {
      if (strcmp(v->callees[i], v->qualified) == 0) {
        self_edge = 1;
      }
    }
    while (state->depth > first) {
      RFunction *w = &reflect->functions[state->stack[--state->depth]];
      w->scc_on_stack = 0;
      w->is_recursive = members > 1 || self_edge;
    }
  }
}

static int mark_recursion(Reflect *reflect) {
  Tarjan state;
  state.stack = calloc(reflect->function_count + 1, sizeof(int));
  state.depth = 0;
  state.next_index = 0;
  if (!state.stack) {
    return 0;
  }
  for (size_t i = 0; i < reflect->function_count; i++) {
    reflect->functions[i].scc_index = -1;
    reflect->functions[i].scc_lowlink = -1;
    reflect->functions[i].scc_on_stack = 0;
  }
  for (size_t i = 0; i < reflect->function_count; i++) {
    if (reflect->functions[i].scc_index < 0) {
      tarjan_visit(reflect, &state, &reflect->functions[i]);
    }
  }
  free(state.stack);
  return 1;
}

static size_t image_reserve(Image *image, size_t size, size_t align) {
  size_t offset = image->size;
  if (align > 1) {
    offset = (offset + align - 1) / align * align;
  }
  size_t end = offset + size;
  if (end > image->capacity) {
    size_t next = image->capacity ? image->capacity : 4096;
    while (next < end) {
      next *= 2;
    }
    unsigned char *grown = realloc(image->bytes, next);
    if (!grown) {
      image->failed = 1;
      return 0;
    }
    memset(grown + image->capacity, 0, next - image->capacity);
    image->bytes = grown;
    image->capacity = next;
  }
  image->size = end;
  return offset;
}

static void image_put_u64(Image *image, size_t offset, unsigned long long v) {
  if (offset + 8 <= image->size) {
    memcpy(image->bytes + offset, &v, 8);
  }
}

static void image_put_bool(Image *image, size_t offset, int v) {
  if (offset < image->size) {
    image->bytes[offset] = v ? 1 : 0;
  }
}

static void image_put_pointer(Image *image, size_t offset, size_t target) {
  image_put_u64(image, offset, (unsigned long long)target);
  if (!grow((void **)&image->pointers, &image->pointer_capacity,
            image->pointer_count, sizeof(size_t))) {
    image->failed = 1;
    return;
  }
  image->pointers[image->pointer_count++] = offset;
}

static void image_put_string(Image *image, size_t offset, const char *text) {
  size_t length = text ? strlen(text) : 0;
  if (length == 0) {
    image_put_u64(image, offset, 0);
    image_put_u64(image, offset + 8, 0);
    return;
  }
  size_t pool_offset = image->pool_size;
  size_t end = pool_offset + length + 1;
  if (end > image->pool_capacity) {
    size_t next = image->pool_capacity ? image->pool_capacity : 4096;
    while (next < end) {
      next *= 2;
    }
    unsigned char *grown = realloc(image->pool, next);
    if (!grown) {
      image->failed = 1;
      return;
    }
    image->pool = grown;
    image->pool_capacity = next;
  }
  memcpy(image->pool + pool_offset, text, length + 1);
  image->pool_size = end;
  if (!grow((void **)&image->string_fields, &image->string_capacity,
            image->string_count, sizeof(size_t))) {
    image->failed = 1;
    return;
  }
  size_t *offsets = realloc(image->string_offsets,
                            image->string_capacity * sizeof(size_t));
  if (!offsets) {
    image->failed = 1;
    return;
  }
  image->string_offsets = offsets;
  image->string_fields[image->string_count] = offset;
  image->string_offsets[image->string_count] = pool_offset;
  image->string_count++;
  image_put_u64(image, offset + 8, (unsigned long long)length);
}

static const Type *lookup_qualified(TypeChecker *checker,
                                    const char *qualified) {
  for (size_t i = 0; i < checker->type_table_count; i++) {
    const Type *type = checker->type_table[i];
    if (type && type->qualified_name &&
        strcmp(type->qualified_name, qualified) == 0) {
      return type;
    }
  }
  return NULL;
}

static size_t field_offset(const Type *type, const char *field, Image *image) {
  int index = type ? type_get_field_index(type, field) : -1;
  if (index < 0 || !type->field_offsets) {
    image->failed = 1;
    return 0;
  }
  return type->field_offsets[index];
}

static void put_site(Image *image, const Type *site_type, size_t base,
                     const IRRuleSite *site) {
  image_put_string(image, base + field_offset(site_type, "file", image),
                   site->file);
  image_put_u64(image, base + field_offset(site_type, "line", image),
                (unsigned long long)site->line);
  image_put_u64(image, base + field_offset(site_type, "column", image),
                (unsigned long long)site->column);
}

static size_t put_string_array(Image *image, const char *const *items,
                               size_t count) {
  if (count == 0) {
    return 0;
  }
  size_t base = image_reserve(image, count * 16, 8);
  for (size_t i = 0; i < count; i++) {
    image_put_string(image, base + i * 16, items[i]);
  }
  return base;
}

static void put_slice(Image *image, size_t offset, size_t target,
                      size_t count) {
  if (count == 0 || target == 0) {
    image_put_u64(image, offset, 0);
    image_put_u64(image, offset + 8, 0);
    return;
  }
  image_put_pointer(image, offset, target);
  image_put_u64(image, offset + 8, (unsigned long long)count);
}

static int add_site(IRRuleImage *out, size_t *capacity, IRRuleSite site) {
  if (!grow((void **)&out->sites, capacity, out->site_count,
            sizeof(IRRuleSite))) {
    return 0;
  }
  out->sites[out->site_count++] = site;
  return 1;
}

static const char *type_display_name(const Type *type) {
  if (!type) {
    return "?";
  }
  if (type->qualified_name && strchr(type->qualified_name, '/')) {
    return type->qualified_name;
  }
  return strip_import_prefix(type->name);
}

static int build_image(Reflect *reflect, const char *root_file,
                       const char *target, IRRuleImage *out,
                       char **error_message) {
  TypeChecker *checker = reflect->checker;
  const Type *program_type = lookup_qualified(checker, "std/rule.Program");
  const Type *function_type = lookup_qualified(checker, "std/rule.Function");
  const Type *type_info_type = lookup_qualified(checker, "std/rule.TypeInfo");
  const Type *field_info_type =
      lookup_qualified(checker, "std/rule.FieldInfo");
  const Type *site_type = lookup_qualified(checker, "std/rule.Site");
  const Type *effect_info_type =
      lookup_qualified(checker, "std/rule.EffectInfo");
  const Type *global_info_type =
      lookup_qualified(checker, "std/rule.GlobalInfo");
  if (!program_type || !function_type || !type_info_type ||
      !field_info_type || !site_type || !effect_info_type ||
      !global_info_type) {
    *error_message = strdup("a @rule needs the Program, Function, TypeInfo, "
                            "FieldInfo, EffectInfo, GlobalInfo and Site "
                            "records from std/rule");
    return 0;
  }
  Image image;
  memset(&image, 0, sizeof(image));
  size_t site_capacity = 0;

  size_t program_offset = image_reserve(&image, program_type->size,
                                        program_type->alignment);
  size_t functions_offset =
      reflect->function_count
          ? image_reserve(&image, function_type->size * reflect->function_count,
                          function_type->alignment)
          : 0;
  size_t types_offset =
      reflect->type_count
          ? image_reserve(&image, type_info_type->size * reflect->type_count,
                          type_info_type->alignment)
          : 0;

  for (size_t i = 0; i < reflect->function_count && !image.failed; i++) {
    RFunction *fn = &reflect->functions[i];
    size_t base = functions_offset + i * function_type->size;
    image_put_string(&image, base + field_offset(function_type, "name", &image),
                     fn->name);
    image_put_string(&image,
                     base + field_offset(function_type, "qualified", &image),
                     fn->qualified);
    image_put_string(&image,
                     base + field_offset(function_type, "module", &image),
                     fn->module);
    put_site(&image, site_type,
             base + field_offset(function_type, "site", &image), &fn->site);
    size_t callees = put_string_array(&image, fn->callees, fn->callee_count);
    put_slice(&image, base + field_offset(function_type, "callees", &image),
              callees, fn->callee_count);
    size_t matches = put_string_array(&image, fn->matches, fn->match_count);
    put_slice(&image, base + field_offset(function_type, "matches", &image),
              matches, fn->match_count);
    size_t params =
        put_string_array(&image, fn->param_types, fn->param_count);
    put_slice(&image,
              base + field_offset(function_type, "param_types", &image),
              params, fn->param_count);
    image_put_string(&image,
                     base + field_offset(function_type, "return_type", &image),
                     fn->return_type);
    image_put_bool(&image,
                   base + field_offset(function_type, "is_extern", &image),
                   fn->is_extern);
    image_put_bool(&image,
                   base + field_offset(function_type, "is_exported", &image),
                   fn->is_exported);
    image_put_bool(&image,
                   base + field_offset(function_type, "is_recursive", &image),
                   fn->is_recursive);
    image_put_bool(
        &image, base + field_offset(function_type, "is_address_taken", &image),
        fn->is_address_taken);
    image_put_bool(
        &image,
        base + field_offset(function_type, "has_indirect_calls", &image),
        fn->has_indirect_calls);
    image_put_bool(&image,
                   base + field_offset(function_type, "is_noalloc", &image),
                   fn->is_noalloc);
    image_put_bool(&image,
                   base + field_offset(function_type, "is_pure", &image),
                   fn->is_pure);
    image_put_bool(&image,
                   base + field_offset(function_type, "is_inline", &image),
                   fn->is_inline);
    image_put_bool(&image,
                   base + field_offset(function_type, "is_swappable", &image),
                   fn->is_swappable);
    image_put_bool(&image,
                   base + field_offset(function_type, "is_kernel", &image),
                   fn->is_kernel);
    {
      size_t effects = put_string_array(&image, fn->effects, fn->effect_count);
      put_slice(&image, base + field_offset(function_type, "effects", &image),
                effects, fn->effect_count);
      size_t requires =
          put_string_array(&image, fn->requires, fn->require_count);
      put_slice(&image, base + field_offset(function_type, "requires", &image),
                requires, fn->require_count);
      size_t forbids = put_string_array(&image, fn->forbids, fn->forbid_count);
      put_slice(&image, base + field_offset(function_type, "forbids", &image),
                forbids, fn->forbid_count);
      size_t provides =
          put_string_array(&image, fn->provides, fn->provide_count);
      put_slice(&image, base + field_offset(function_type, "provides", &image),
                provides, fn->provide_count);
      {
        size_t allocations =
            fn->allocation_count
                ? image_reserve(&image, site_type->size * fn->allocation_count,
                                site_type->alignment)
                : 0;
        for (size_t a = 0; a < fn->allocation_count; a++) {
          put_site(&image, site_type, allocations + a * site_type->size,
                   &fn->allocations[a]);
        }
        put_slice(&image,
                  base + field_offset(function_type, "allocations", &image),
                  allocations, fn->allocation_count);
      }
      {
        size_t frees = fn->free_count
                           ? image_reserve(&image,
                                           site_type->size * fn->free_count,
                                           site_type->alignment)
                           : 0;
        for (size_t a = 0; a < fn->free_count; a++) {
          put_site(&image, site_type, frees + a * site_type->size,
                   &fn->frees[a]);
        }
        put_slice(&image, base + field_offset(function_type, "frees", &image),
                  frees, fn->free_count);
      }
      {
        size_t writes =
            put_string_array(&image, fn->writes_globals, fn->write_count);
        put_slice(&image,
                  base + field_offset(function_type, "writes_globals", &image),
                  writes, fn->write_count);
      }
      image_put_bool(
          &image, base + field_offset(function_type, "returns_pointer", &image),
          fn->returns_pointer);
    }
    if (!add_site(out, &site_capacity, fn->site)) {
      image.failed = 1;
    }
    for (size_t a = 0; a < fn->allocation_count && !image.failed; a++) {
      if (!add_site(out, &site_capacity, fn->allocations[a])) {
        image.failed = 1;
      }
    }
    for (size_t a = 0; a < fn->free_count && !image.failed; a++) {
      if (!add_site(out, &site_capacity, fn->frees[a])) {
        image.failed = 1;
      }
    }
  }

  size_t globals_offset = 0;
  if (reflect->global_count > 0) {
    globals_offset =
        image_reserve(&image, global_info_type->size * reflect->global_count,
                      global_info_type->alignment);
    for (size_t i = 0; i < reflect->global_count && !image.failed; i++) {
      RGlobal *global = &reflect->globals[i];
      size_t base = globals_offset + i * global_info_type->size;
      size_t writers =
          put_string_array(&image, global->written_by, global->written_count);
      image_put_string(&image,
                       base + field_offset(global_info_type, "name", &image),
                       global->name);
      image_put_string(&image,
                       base + field_offset(global_info_type, "module", &image),
                       global->module);
      put_site(&image, site_type,
               base + field_offset(global_info_type, "site", &image),
               &global->site);
      image_put_string(
          &image, base + field_offset(global_info_type, "type_name", &image),
          global->type_name);
      put_slice(&image,
                base + field_offset(global_info_type, "written_by", &image),
                writers, global->written_count);
      image_put_bool(
          &image,
          base + field_offset(global_info_type, "address_taken", &image),
          global->address_taken);
      if (!add_site(out, &site_capacity, global->site)) {
        image.failed = 1;
      }
    }
  }
  put_slice(&image,
            program_offset + field_offset(program_type, "globals", &image),
            globals_offset, reflect->global_count);

  size_t effects_offset = 0;
  if (checker->effect_count > 0) {
    effects_offset =
        image_reserve(&image, effect_info_type->size * checker->effect_count,
                      effect_info_type->alignment);
    for (size_t i = 0; i < checker->effect_count && !image.failed; i++) {
      const TypeCheckerEffect *effect = &checker->effects[i];
      size_t base = effects_offset + i * effect_info_type->size;
      IRRuleSite site;
      site.file = effect->site.filename;
      site.line = effect->site.line;
      site.column = effect->site.column;
      image_put_string(&image,
                       base + field_offset(effect_info_type, "name", &image),
                       effect->name);
      image_put_string(&image,
                       base + field_offset(effect_info_type, "module", &image),
                       effect->is_builtin ? "" : module_of(effect->site.filename));
      put_site(&image, site_type,
               base + field_offset(effect_info_type, "site", &image), &site);
      image_put_bool(&image,
                     base + field_offset(effect_info_type, "is_builtin", &image),
                     effect->is_builtin);
      if (!effect->is_builtin && !add_site(out, &site_capacity, site)) {
        image.failed = 1;
      }
    }
  }
  put_slice(&image,
            program_offset + field_offset(program_type, "effects", &image),
            effects_offset, checker->effect_count);

  for (size_t i = 0; i < reflect->type_count && !image.failed; i++) {
    RType *entry = &reflect->types[i];
    const Type *type = entry->type;
    size_t base = types_offset + i * type_info_type->size;
    image_put_string(&image,
                     base + field_offset(type_info_type, "name", &image),
                     entry->name);
    image_put_string(&image,
                     base + field_offset(type_info_type, "qualified", &image),
                     entry->qualified);
    image_put_string(&image,
                     base + field_offset(type_info_type, "module", &image),
                     entry->module);
    put_site(&image, site_type,
             base + field_offset(type_info_type, "site", &image),
             &entry->site);
    image_put_string(&image,
                     base + field_offset(type_info_type, "kind", &image),
                     entry->kind);
    image_put_string(&image,
                     base + field_offset(type_info_type, "base", &image),
                     entry->base);
    image_put_u64(&image, base + field_offset(type_info_type, "size", &image),
                  (unsigned long long)entry->size);
    image_put_u64(&image, base + field_offset(type_info_type, "align", &image),
                  (unsigned long long)entry->align);
    image_put_u64(&image,
                  base + field_offset(type_info_type, "layout", &image),
                  (unsigned long long)entry->layout);
    size_t field_count = type->kind == TYPE_STRUCT ? type->field_count : 0;
    size_t fields = 0;
    if (field_count > 0) {
      fields = image_reserve(&image, field_info_type->size * field_count,
                             field_info_type->alignment);
      for (size_t f = 0; f < field_count; f++) {
        size_t fbase = fields + f * field_info_type->size;
        image_put_string(&image,
                         fbase + field_offset(field_info_type, "name", &image),
                         type->field_names ? type->field_names[f] : "");
        image_put_string(
            &image, fbase + field_offset(field_info_type, "type_name", &image),
            type_display_name(type->field_types ? type->field_types[f]
                                                : NULL));
        image_put_u64(&image,
                      fbase + field_offset(field_info_type, "offset", &image),
                      type->field_offsets ? type->field_offsets[f] : 0);
        image_put_u64(&image,
                      fbase + field_offset(field_info_type, "size", &image),
                      type->field_types && type->field_types[f]
                          ? type->field_types[f]->size
                          : 0);
        image_put_u64(&image,
                      fbase + field_offset(field_info_type, "align", &image),
                      type->field_types && type->field_types[f]
                          ? type->field_types[f]->alignment
                          : 0);
        image_put_u64(&image,
                      fbase +
                          field_offset(field_info_type, "pointee_align", &image),
                      type->field_types && type->field_types[f] &&
                              type->field_types[f]->kind == TYPE_POINTER &&
                              type->field_types[f]->base_type
                          ? type->field_types[f]->base_type->alignment
                          : 0);
      }
    }
    put_slice(&image, base + field_offset(type_info_type, "fields", &image),
              fields, field_count);
    const char *const *variant_names = NULL;
    size_t variant_count = 0;
    if (type->kind == TYPE_ENUM) {
      variant_names = (const char *const *)type->enum_member_names;
      variant_count = type->enum_member_count;
    } else if (type->kind == TYPE_TAGGED_ENUM) {
      variant_names = (const char *const *)type->tagged_variant_names;
      variant_count = type->tagged_variant_count;
    }
    size_t variants = 0;
    if (variant_count > 0 && variant_names) {
      const char **shown = calloc(variant_count, sizeof(const char *));
      if (!shown) {
        image.failed = 1;
        break;
      }
      for (size_t v = 0; v < variant_count; v++) {
        shown[v] = strip_import_prefix(variant_names[v]);
      }
      variants = put_string_array(&image, shown, variant_count);
      free(shown);
    }
    put_slice(&image, base + field_offset(type_info_type, "variants", &image),
              variants, variant_count);
    if (!add_site(out, &site_capacity, entry->site)) {
      image.failed = 1;
    }
  }

  size_t modules =
      put_string_array(&image, reflect->modules, reflect->module_count);

  put_slice(&image, program_offset + field_offset(program_type, "functions",
                                                   &image),
            functions_offset, reflect->function_count);
  put_slice(&image,
            program_offset + field_offset(program_type, "types", &image),
            types_offset, reflect->type_count);
  put_slice(&image,
            program_offset + field_offset(program_type, "modules", &image),
            modules, reflect->module_count);
  image_put_string(&image,
                   program_offset + field_offset(program_type, "target", &image),
                   target ? target : "");
  image_put_string(&image,
                   program_offset + field_offset(program_type, "file", &image),
                   root_file ? root_file : "");

  size_t pool_base = image_reserve(&image, image.pool_size, 8);
  if (!image.failed && image.pool_size > 0) {
    memcpy(image.bytes + pool_base, image.pool, image.pool_size);
  }
  for (size_t i = 0; i < image.string_count && !image.failed; i++) {
    image_put_pointer(&image, image.string_fields[i],
                      pool_base + image.string_offsets[i]);
  }

  free(image.pool);
  free(image.string_fields);
  free(image.string_offsets);
  if (image.failed) {
    free(image.bytes);
    free(image.pointers);
    *error_message = strdup("could not lay out the program the rules read");
    return 0;
  }
  out->bytes = image.bytes;
  out->size = image.size;
  out->pointer_offsets = image.pointers;
  out->pointer_count = image.pointer_count;
  out->function_count = reflect->function_count;
  out->type_count = reflect->type_count;
  return 1;
}

IRFunction *ir_program_find_function(IRProgram *program, const char *name);

typedef struct {
  char *name;
  const char *type;
  int is_parameter;
} MetaLocal;

typedef struct {
  size_t lo;
  size_t hi;
  const IRInstruction *term;
} MetaBlock;

typedef struct {
  Image *image;
  TypeChecker *checker;
  IRProgram *ir;
  const Type *value_type;
  const Type *op_type;
  const Type *node_type;
  const Type *local_type;
  const Type *function_type;
  const Type *site_type;
  IRFunction *fn;
  MetaLocal *locals;
  size_t local_count;
  MetaLocal *temps;
  size_t temp_count;
  MetaBlock *blocks;
  size_t block_count;
} MetaImage;

static const char *meta_find_type(const MetaLocal *entries, size_t count,
                                  const char *name) {
  for (size_t i = 0; i < count; i++) {
    if (strcmp(entries[i].name, name) == 0) {
      return entries[i].type;
    }
  }
  return NULL;
}

static int meta_add_entry(MetaLocal **entries, size_t *count, const char *name,
                          const char *type, int is_parameter) {
  if (meta_find_type(*entries, *count, name)) {
    return 1;
  }
  MetaLocal *grown = realloc(*entries, (*count + 1) * sizeof(MetaLocal));
  if (!grown) {
    return 0;
  }
  *entries = grown;
  (*entries)[*count].name = strdup(name);
  (*entries)[*count].type = type;
  (*entries)[*count].is_parameter = is_parameter;
  (*count)++;
  return 1;
}

static const char *meta_float_name(int bits) {
  return bits == 32 ? "float32" : "float64";
}

static const char *meta_operand_type(MetaImage *m, const IROperand *operand) {
  const char *type = NULL;
  switch (operand->kind) {
  case IR_OPERAND_INT:
    return "int64";
  case IR_OPERAND_FLOAT:
    return meta_float_name(operand->float_bits);
  case IR_OPERAND_STRING:
    return "string";
  case IR_OPERAND_TEMP:
    type = operand->name
               ? meta_find_type(m->temps, m->temp_count, operand->name)
               : NULL;
    return type ? type : "int64";
  case IR_OPERAND_SYMBOL:
    type = operand->name
               ? meta_find_type(m->locals, m->local_count, operand->name)
               : NULL;
    if (!type && operand->name) {
      Symbol *symbol =
          symbol_table_lookup(m->checker->symbol_table, operand->name);
      if (symbol && symbol->type && symbol->type->name &&
          symbol->kind != SYMBOL_FUNCTION) {
        return symbol->type->name;
      }
    }
    return type ? type : "int64";
  default:
    return "int64";
  }
}

static Symbol *meta_callee(MetaImage *m, const IRInstruction *call) {
  Symbol *symbol =
      call->text ? symbol_table_lookup(m->checker->symbol_table, call->text)
                 : NULL;
  return symbol && symbol->kind == SYMBOL_FUNCTION ? symbol : NULL;
}

static const char *meta_result_type(MetaImage *m, const IRInstruction *ins) {
  switch (ins->op) {
  case IR_OP_BINARY:
  case IR_OP_UNARY:
    if (ins->is_float && ins->text && ins->text[0] != '<' &&
        ins->text[0] != '>' && ins->text[0] != '=' && ins->text[0] != '!') {
      return meta_float_name(ins->float_bits);
    }
    return "int64";
  case IR_OP_LOAD:
    return ins->is_float ? meta_float_name(ins->float_bits) : "int64";
  case IR_OP_CAST:
    if (ins->text && strcmp(ins->text, "float32") == 0) {
      return "float32";
    }
    if (ins->text && strcmp(ins->text, "float64") == 0) {
      return "float64";
    }
    return "int64";
  case IR_OP_CALL: {
    Symbol *callee = meta_callee(m, ins);
    Type *returned = callee ? callee->data.function.return_type : NULL;
    if (returned && returned->name &&
        (returned->kind == TYPE_FLOAT32 || returned->kind == TYPE_FLOAT64 ||
         returned->kind == TYPE_STRUCT || returned->kind == TYPE_STRING ||
         returned->kind == TYPE_SLICE || returned->kind == TYPE_ARRAY)) {
      return returned->name;
    }
    return "int64";
  }
  case IR_OP_ASSIGN:
    return meta_operand_type(m, &ins->lhs);
  default:
    return "int64";
  }
}

static int meta_is_terminator(IROpcode op) {
  return op == IR_OP_JUMP || op == IR_OP_BRANCH_ZERO ||
         op == IR_OP_BRANCH_EQ || op == IR_OP_RETURN;
}

static int meta_split_blocks(MetaImage *m) {
  IRFunction *fn = m->fn;
  size_t count = fn->instruction_count;
  size_t *starts = calloc(count + 2, sizeof(size_t));
  size_t start_count = 0;
  if (!starts) {
    return 0;
  }
  starts[start_count++] = 0;
  for (size_t i = 0; i < count; i++) {
    IROpcode op = fn->instructions[i].op;
    if (op == IR_OP_LABEL && starts[start_count - 1] != i) {
      starts[start_count++] = i;
    }
    if (meta_is_terminator(op) && i + 1 < count &&
        starts[start_count - 1] != i + 1) {
      starts[start_count++] = i + 1;
    }
  }
  m->blocks = calloc(start_count + 1, sizeof(MetaBlock));
  m->block_count = 0;
  if (!m->blocks) {
    free(starts);
    return 0;
  }
  for (size_t k = 0; k < start_count; k++) {
    size_t lo = starts[k];
    size_t hi = k + 1 < start_count ? starts[k + 1] : count;
    if (lo >= hi) {
      continue;
    }
    MetaBlock *block = &m->blocks[m->block_count++];
    block->lo = lo;
    block->hi = hi;
    block->term = meta_is_terminator(fn->instructions[hi - 1].op)
                      ? &fn->instructions[hi - 1]
                      : NULL;
  }
  if (m->block_count == 0) {
    m->block_count = 1;
  }
  free(starts);
  return 1;
}

static long meta_block_of_label(const MetaImage *m, const char *label) {
  for (size_t b = 0; b < m->block_count; b++) {
    const IRInstruction *first = &m->fn->instructions[m->blocks[b].lo];
    if (m->blocks[b].hi > m->blocks[b].lo && first->op == IR_OP_LABEL &&
        first->text && label && strcmp(first->text, label) == 0) {
      return (long)b;
    }
  }
  return -1;
}

static int meta_build_cfg(MetaImage *m, GpuStructure *structure, char *why,
                          size_t why_size) {
  GpuCfgNode *cfg = calloc(m->block_count + 1, sizeof(GpuCfgNode));
  int ok;
  if (!cfg) {
    return 0;
  }
  for (size_t b = 0; b < m->block_count; b++) {
    const IRInstruction *term = m->blocks[b].term;
    size_t next = b + 1;
    if (term && term->op == IR_OP_RETURN) {
      cfg[b].exit = GPU_CFG_RETURN;
    } else if (term) {
      long target = meta_block_of_label(m, term->text);
      if (target < 0) {
        snprintf(why, why_size, "a jump goes to the unknown label '%s'",
                 term->text ? term->text : "?");
        free(cfg);
        return 0;
      }
      if (term->op == IR_OP_JUMP || next >= m->block_count) {
        cfg[b].exit = GPU_CFG_JUMP;
        cfg[b].target[0] = cfg[b].target[1] = (size_t)target;
      } else {
        cfg[b].exit = GPU_CFG_BRANCH;
        cfg[b].target[0] = (size_t)target;
        cfg[b].target[1] = next;
      }
    } else if (next < m->block_count) {
      cfg[b].exit = GPU_CFG_JUMP;
      cfg[b].target[0] = cfg[b].target[1] = next;
    } else {
      cfg[b].exit = GPU_CFG_RETURN;
    }
  }
  ok = gpu_structure_build(cfg, m->block_count, 0, structure, why, why_size);
  free(cfg);
  return ok;
}

static void meta_spell_name(char *out, size_t size, const char *name) {
  size_t used = (size_t)snprintf(out, size, "v_");
  for (const char *p = name ? name : ""; *p && used + 1 < size; p++) {
    char c = *p;
    int plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '_';
    out[used++] = plain ? c : '_';
  }
  out[used] = '\0';
}

static char *meta_spell_string(const char *text) {
  size_t length = text ? strlen(text) : 0;
  char *out = malloc(length * 4 + 3);
  size_t used = 0;
  if (!out) {
    return NULL;
  }
  out[used++] = '"';
  for (size_t i = 0; i < length; i++) {
    unsigned char c = (unsigned char)text[i];
    if (c == '"' || c == '\\' || c == '{' || c == '}') {
      out[used++] = '\\';
      out[used++] = (char)c;
    } else if (c == '\n') {
      out[used++] = '\\';
      out[used++] = 'n';
    } else if (c == '\t') {
      out[used++] = '\\';
      out[used++] = 't';
    } else {
      out[used++] = (char)c;
    }
  }
  out[used++] = '"';
  out[used] = '\0';
  return out;
}

static void meta_put_value(MetaImage *m, size_t base,
                           const IROperand *operand) {
  Image *image = m->image;
  const Type *t = m->value_type;
  char spelled[320];
  long long kind = 0;
  const char *text = "";
  char *owned = NULL;
  switch (operand->kind) {
  case IR_OPERAND_TEMP:
    kind = 1;
    meta_spell_name(spelled, sizeof(spelled), operand->name);
    text = spelled;
    break;
  case IR_OPERAND_SYMBOL:
    if (operand->name &&
        meta_find_type(m->locals, m->local_count, operand->name)) {
      kind = 2;
      meta_spell_name(spelled, sizeof(spelled), operand->name);
      text = spelled;
    } else {
      kind = 6;
      text = operand->name ? operand->name : "";
    }
    break;
  case IR_OPERAND_INT:
    kind = 3;
    snprintf(spelled, sizeof(spelled), "%lld", operand->int_value);
    text = spelled;
    break;
  case IR_OPERAND_FLOAT:
    kind = 4;
    snprintf(spelled, sizeof(spelled), "%.17g", operand->float_value);
    if (!strchr(spelled, '.') && !strchr(spelled, 'e') &&
        !strchr(spelled, 'n')) {
      size_t digits = strlen(spelled);
      spelled[digits] = '.';
      spelled[digits + 1] = '0';
      spelled[digits + 2] = '\0';
    }
    text = spelled;
    break;
  case IR_OPERAND_STRING:
    kind = 5;
    owned = meta_spell_string(operand->name);
    text = owned ? owned : "\"\"";
    break;
  default:
    break;
  }
  image_put_u64(image, base + field_offset(t, "kind", image),
                (unsigned long long)kind);
  image_put_string(image, base + field_offset(t, "text", image), text);
  const char *type_name = meta_operand_type(m, operand);
  image_put_string(image, base + field_offset(t, "type_name", image),
                   type_name);
  if (kind == 1 || kind == 2 || kind == 6) {
    Type *held = type_checker_get_type_by_name(m->checker, type_name);
    image_put_bool(image, base + field_offset(t, "is_aggregate", image),
                   held && (held->kind == TYPE_STRUCT ||
                            held->kind == TYPE_STRING ||
                            held->kind == TYPE_SLICE ||
                            held->kind == TYPE_ARRAY ||
                            held->kind == TYPE_TAGGED_ENUM));
  }
  free(owned);
}

static void meta_put_op(MetaImage *m, size_t base, const IRInstruction *ins) {
  Image *image = m->image;
  const Type *t = m->op_type;
  image_put_string(image, base + field_offset(t, "op", image),
                   ir_opcode_name(ins->op));
  image_put_string(image, base + field_offset(t, "text", image),
                   ins->text ? ins->text : "");
  meta_put_value(m, base + field_offset(t, "dest", image), &ins->dest);
  meta_put_value(m, base + field_offset(t, "lhs", image), &ins->lhs);
  meta_put_value(m, base + field_offset(t, "rhs", image), &ins->rhs);
  image_put_bool(image, base + field_offset(t, "is_float", image),
                 ins->is_float);
  image_put_u64(image, base + field_offset(t, "float_bits", image),
                (unsigned long long)ins->float_bits);
  image_put_bool(image, base + field_offset(t, "is_unsigned", image),
                 ins->is_unsigned);
  image_put_u64(image, base + field_offset(t, "line", image),
                (unsigned long long)ins->location.line);
  if (ins->argument_count > 0) {
    size_t args = image_reserve(image, m->value_type->size * ins->argument_count,
                                m->value_type->alignment);
    for (size_t a = 0; a < ins->argument_count; a++) {
      meta_put_value(m, args + a * m->value_type->size, &ins->arguments[a]);
    }
    put_slice(image, base + field_offset(t, "args", image), args,
              ins->argument_count);
    Symbol *callee = ins->op == IR_OP_CALL ? meta_callee(m, ins) : NULL;
    if (callee && callee->data.function.parameter_types &&
        callee->data.function.parameter_count == ins->argument_count) {
      const char **names = calloc(ins->argument_count, sizeof(char *));
      if (names) {
        for (size_t a = 0; a < ins->argument_count; a++) {
          Type *param = callee->data.function.parameter_types[a];
          names[a] = param && param->name ? param->name : "int64";
        }
        size_t types = put_string_array(image, names, ins->argument_count);
        put_slice(image, base + field_offset(t, "arg_types", image), types,
                  ins->argument_count);
        free(names);
      }
    }
  }
}

static size_t meta_put_node(MetaImage *m, const GpuSNode *node);
static void meta_fill_node(MetaImage *m, size_t base, const GpuSNode *node);

static void meta_put_child(MetaImage *m, size_t base, const char *field,
                           const GpuSNode *child) {
  if (!child) {
    return;
  }
  size_t at = meta_put_node(m, child);
  put_slice(m->image, base + field_offset(m->node_type, field, m->image), at, 1);
}

static size_t meta_put_node(MetaImage *m, const GpuSNode *node) {
  size_t base = image_reserve(m->image, m->node_type->size,
                              m->node_type->alignment);
  meta_fill_node(m, base, node);
  return base;
}

static void meta_fill_node(MetaImage *m, size_t base, const GpuSNode *node) {
  Image *image = m->image;
  const Type *t = m->node_type;
  image_put_u64(image, base + field_offset(t, "kind", image),
                (unsigned long long)node->kind);
  image_put_u64(image, base + field_offset(t, "value", image),
                (unsigned long long)(long long)node->value);
  switch (node->kind) {
  case GPU_SNODE_SEQ: {
    if (node->child_count == 0) {
      break;
    }
    size_t array = image_reserve(image, t->size * node->child_count, t->alignment);
    for (size_t i = 0; i < node->child_count; i++) {
      meta_fill_node(m, array + i * t->size, node->children[i]);
    }
    put_slice(image, base + field_offset(t, "children", image), array,
              node->child_count);
    break;
  }
  case GPU_SNODE_CODE: {
    const MetaBlock *block = &m->blocks[node->block];
    size_t count = 0;
    for (size_t i = block->lo; i < block->hi; i++) {
      const IRInstruction *ins = &m->fn->instructions[i];
      if (ins != block->term && ins->op != IR_OP_LABEL && ins->op != IR_OP_NOP &&
          ins->op != IR_OP_DECLARE_LOCAL) {
        count++;
      }
    }
    if (count == 0) {
      break;
    }
    size_t ops = image_reserve(image, m->op_type->size * count,
                               m->op_type->alignment);
    size_t at = 0;
    for (size_t i = block->lo; i < block->hi; i++) {
      const IRInstruction *ins = &m->fn->instructions[i];
      if (ins != block->term && ins->op != IR_OP_LABEL && ins->op != IR_OP_NOP &&
          ins->op != IR_OP_DECLARE_LOCAL) {
        meta_put_op(m, ops + at * m->op_type->size, ins);
        at++;
      }
    }
    put_slice(image, base + field_offset(t, "ops", image), ops, count);
    break;
  }
  case GPU_SNODE_IF: {
    const IRInstruction *term = m->blocks[node->block].term;
    if (term) {
      meta_put_op(m, base + field_offset(t, "condition", image), term);
    }
    meta_put_child(m, base, "then_body", node->then_node);
    meta_put_child(m, base, "else_body", node->else_node);
    break;
  }
  case GPU_SNODE_LOOP:
  case GPU_SNODE_BLOCK:
    meta_put_child(m, base, "body", node->body);
    break;
  case GPU_SNODE_RETURN: {
    const IRInstruction *term = m->blocks[node->block].term;
    if (term && term->op == IR_OP_RETURN) {
      meta_put_value(m, base + field_offset(t, "returned", image), &term->lhs);
    }
    break;
  }
  case GPU_SNODE_CHECK:
    if (node->case_count > 0) {
      size_t values = image_reserve(image, 8 * node->case_count, 8);
      size_t actions = image_reserve(image, 8 * node->case_count, 8);
      for (size_t c = 0; c < node->case_count; c++) {
        image_put_u64(image, values + c * 8,
                      (unsigned long long)(long long)node->cases[c].value);
        image_put_u64(image, actions + c * 8,
                      (unsigned long long)node->cases[c].action);
      }
      put_slice(image, base + field_offset(t, "case_values", image), values,
                node->case_count);
      put_slice(image, base + field_offset(t, "case_actions", image), actions,
                node->case_count);
    }
    break;
  default:
    break;
  }
}

static size_t meta_put_locals(MetaImage *m, const MetaLocal *entries,
                              size_t count, size_t *out_kept, int parameters) {
  Image *image = m->image;
  size_t kept = 0;
  for (size_t i = 0; i < count; i++) {
    if (parameters < 0 || entries[i].is_parameter == parameters) {
      kept++;
    }
  }
  *out_kept = kept;
  if (kept == 0) {
    return 0;
  }
  size_t array = image_reserve(image, m->local_type->size * kept,
                               m->local_type->alignment);
  size_t at = 0;
  for (size_t i = 0; i < count; i++) {
    char spelled[320];
    size_t base;
    if (parameters >= 0 && entries[i].is_parameter != parameters) {
      continue;
    }
    base = array + at * m->local_type->size;
    meta_spell_name(spelled, sizeof(spelled), entries[i].name);
    image_put_string(image, base + field_offset(m->local_type, "name", image),
                     spelled);
    image_put_string(image,
                     base + field_offset(m->local_type, "source_name", image),
                     entries[i].name);
    image_put_string(image, base + field_offset(m->local_type, "type_name", image),
                     entries[i].type);
    at++;
  }
  return array;
}

static void meta_put_function(MetaImage *m, size_t base, IRFunction *fn,
                              const char *inner_name) {
  Image *image = m->image;
  const Type *t = m->function_type;
  char why[400];
  GpuStructure structure;
  m->fn = fn;
  m->locals = NULL;
  m->local_count = 0;
  m->temps = NULL;
  m->temp_count = 0;
  memset(&structure, 0, sizeof(structure));
  image_put_string(image, base + field_offset(t, "name", image),
                   strip_import_prefix(fn->name));
  image_put_string(image, base + field_offset(t, "ir_name", image), fn->name);
  image_put_string(image, base + field_offset(t, "inner_name", image),
                   inner_name ? inner_name : "");
  image_put_string(image, base + field_offset(t, "return_type", image),
                   fn->return_type_name ? fn->return_type_name : "void");
  image_put_bool(image, base + field_offset(t, "is_pure_declared", image),
                 fn->is_pure);
  put_site(image, m->site_type, base + field_offset(t, "site", image),
           &(IRRuleSite){fn->location.filename, fn->location.line,
                         fn->location.column});
  for (size_t p = 0; p < fn->parameter_count; p++) {
    meta_add_entry(&m->locals, &m->local_count, fn->parameter_names[p],
                   fn->parameter_types[p], 1);
  }
  for (size_t i = 0; i < fn->instruction_count; i++) {
    const IRInstruction *ins = &fn->instructions[i];
    if (ins->op == IR_OP_DECLARE_LOCAL && ins->dest.name) {
      meta_add_entry(&m->locals, &m->local_count, ins->dest.name,
                     ins->text ? ins->text : "int64", 0);
    }
  }
  for (size_t i = 0; i < fn->instruction_count; i++) {
    const IRInstruction *ins = &fn->instructions[i];
    if (ins->dest.kind == IR_OPERAND_TEMP && ins->dest.name) {
      meta_add_entry(&m->temps, &m->temp_count, ins->dest.name,
                     meta_result_type(m, ins), 0);
    }
  }
  size_t kept = 0;
  size_t params = meta_put_locals(m, m->locals, m->local_count, &kept, 1);
  put_slice(image, base + field_offset(t, "params", image), params, kept);
  size_t locals = meta_put_locals(m, m->locals, m->local_count, &kept, 0);
  put_slice(image, base + field_offset(t, "locals", image), locals, kept);
  size_t temps = meta_put_locals(m, m->temps, m->temp_count, &kept, -1);
  put_slice(image, base + field_offset(t, "temps", image), temps, kept);
  if (!meta_split_blocks(m) ||
      !meta_build_cfg(m, &structure, why, sizeof(why))) {
    image_put_string(image, base + field_offset(t, "refused", image), why);
    return;
  }
  size_t body = meta_put_node(m, structure.root);
  put_slice(image, base + field_offset(t, "body", image), body, 1);
  image_put_bool(image, base + field_offset(t, "uses_exit", image),
                 structure.uses_exit);
}

static void meta_collect_reached(IRProgram *ir, IRFunction *fn,
                                 IRFunction ***out, size_t *count) {
  for (size_t i = 0; i < fn->instruction_count; i++) {
    const IRInstruction *ins = &fn->instructions[i];
    int seen = 0;
    if (ins->op != IR_OP_CALL || !ins->text) {
      continue;
    }
    IRFunction *callee = ir_program_find_function(ir, ins->text);
    if (!callee || callee == fn) {
      continue;
    }
    for (size_t k = 0; k < *count; k++) {
      if ((*out)[k] == callee) {
        seen = 1;
      }
    }
    if (seen) {
      continue;
    }
    IRFunction **grown = realloc(*out, (*count + 1) * sizeof(IRFunction *));
    if (!grown) {
      return;
    }
    *out = grown;
    (*out)[(*count)++] = callee;
    meta_collect_reached(ir, callee, out, count);
  }
}

int rule_reflect_build_function(TypeChecker *checker, IRProgram *ir,
                                const char *name, const char *inner_name,
                                IRRuleImage *out, char **error_message) {
  MetaImage m;
  Image image;
  memset(&m, 0, sizeof(m));
  memset(&image, 0, sizeof(image));
  memset(out, 0, sizeof(*out));
  m.image = &image;
  m.checker = checker;
  m.ir = ir;
  m.value_type = lookup_qualified(checker, "std/meta.IRValue");
  m.op_type = lookup_qualified(checker, "std/meta.IROp");
  m.node_type = lookup_qualified(checker, "std/meta.IRNode");
  m.local_type = lookup_qualified(checker, "std/meta.LocalInfo");
  m.function_type = lookup_qualified(checker, "std/meta.FunctionInfo");
  m.site_type = lookup_qualified(checker, "std/rule.Site");
  if (!m.value_type || !m.op_type || !m.node_type || !m.local_type ||
      !m.function_type || !m.site_type) {
    *error_message = strdup("a generator needs 'import \"std/meta\";'");
    return 0;
  }
  IRFunction *fn = ir_program_find_function(ir, name);
  if (!fn) {
    char message[300];
    snprintf(message, sizeof(message),
             "'%s' has no body for a generator to read", name);
    *error_message = strdup(message);
    return 0;
  }
  size_t root = image_reserve(&image, m.function_type->size,
                              m.function_type->alignment);
  meta_put_function(&m, root, fn, inner_name);
  IRFunction **reached = NULL;
  size_t reached_count = 0;
  meta_collect_reached(ir, fn, &reached, &reached_count);
  if (reached_count > 0) {
    size_t array = image_reserve(&image, m.function_type->size * reached_count,
                                 m.function_type->alignment);
    for (size_t r = 0; r < reached_count; r++) {
      meta_put_function(&m, array + r * m.function_type->size, reached[r], NULL);
    }
    put_slice(&image, root + field_offset(m.function_type, "reached", &image),
              array, reached_count);
  }
  free(reached);
  size_t pool_base = image_reserve(&image, image.pool_size, 8);
  if (!image.failed && image.pool_size > 0) {
    memcpy(image.bytes + pool_base, image.pool, image.pool_size);
  }
  for (size_t i = 0; i < image.string_count && !image.failed; i++) {
    image_put_pointer(&image, image.string_fields[i],
                      pool_base + image.string_offsets[i]);
  }
  free(image.pool);
  free(image.string_fields);
  free(image.string_offsets);
  if (image.failed) {
    free(image.bytes);
    free(image.pointers);
    *error_message = strdup("could not lay out the function a generator reads");
    return 0;
  }
  out->bytes = image.bytes;
  out->size = image.size;
  out->pointer_offsets = image.pointers;
  out->pointer_count = image.pointer_count;
  return 1;
}

int rule_reflect_build_trace(TypeChecker *checker, const char *root_file,
                             IRRuleImage *out, char **error_message) {
  const Type *trace_type = lookup_qualified(checker, "std/rule.Trace");
  const Type *event_type = lookup_qualified(checker, "std/rule.Event");
  const Type *site_type = lookup_qualified(checker, "std/rule.Site");
  Image image;
  size_t trace_offset;
  size_t events_offset;
  size_t count = ir_trace_event_count();
  size_t site_capacity = 0;
  if (!checker || !out || !error_message) {
    return 0;
  }
  memset(out, 0, sizeof(*out));
  *error_message = NULL;
  if (!trace_type || !event_type || !site_type) {
    *error_message = strdup("a @rule over a trace needs the Trace, Event and "
                            "Site records from std/rule");
    return 0;
  }
  memset(&image, 0, sizeof(image));
  trace_offset = image_reserve(&image, trace_type->size, trace_type->alignment);
  events_offset = count ? image_reserve(&image, event_type->size * count,
                                        event_type->alignment)
                        : 0;
  for (size_t i = 0; i < count && !image.failed; i++) {
    const IRTraceEvent *event = ir_trace_event_at(i);
    size_t base = events_offset + i * event_type->size;
    IRRuleSite site;
    image_put_string(&image, base + field_offset(event_type, "kind", &image),
                     event->kind);
    image_put_string(&image, base + field_offset(event_type, "name", &image),
                     event->name);
    site.file = event->file && event->file[0] ? event->file : root_file;
    site.line = event->line;
    site.column = event->column;
    put_site(&image, site_type, base + field_offset(event_type, "site", &image),
             &site);
    image_put_u64(&image, base + field_offset(event_type, "value", &image),
                  (unsigned long long)event->value);
    if (!add_site(out, &site_capacity, site)) {
      image.failed = 1;
    }
  }
  put_slice(&image, trace_offset + field_offset(trace_type, "events", &image),
            events_offset, count);
  image_put_string(&image,
                   trace_offset + field_offset(trace_type, "test", &image),
                   ir_trace_test_name());
  image_put_string(&image,
                   trace_offset + field_offset(trace_type, "file", &image),
                   root_file ? root_file : "");
  {
    size_t pool_base = image.size;
    if (image.pool_size > 0) {
      size_t total = image.size + image.pool_size;
      unsigned char *joined = realloc(image.bytes, total ? total : 1);
      if (!joined) {
        image.failed = 1;
      } else {
        memcpy(joined + image.size, image.pool, image.pool_size);
        image.bytes = joined;
        image.size = total;
      }
    }
    for (size_t i = 0; i < image.string_count && !image.failed; i++) {
      image_put_pointer(&image, image.string_fields[i],
                        pool_base + image.string_offsets[i]);
    }
  }
  free(image.pool);
  free(image.string_fields);
  free(image.string_offsets);
  if (image.failed) {
    free(image.bytes);
    free(image.pointers);
    free(out->sites);
    memset(out, 0, sizeof(*out));
    *error_message = strdup("could not lay out the trace the rules read");
    return 0;
  }
  out->bytes = image.bytes;
  out->size = image.size;
  out->pointer_offsets = image.pointers;
  out->pointer_count = image.pointer_count;
  out->function_count = 0;
  out->type_count = 0;
  return 1;
}

static int machine_name_is_rule(const IRProgram *program, const char *name) {
  if (!program || !name) {
    return 0;
  }
  for (size_t i = 0; i < program->function_count; i++) {
    const IRFunction *fn = program->functions[i];
    if (fn && fn->is_rule && fn->name && strcmp(fn->name, name) == 0) {
      return 1;
    }
  }
  return 0;
}

int rule_reflect_build_machine(TypeChecker *checker, const IRProgram *program,
                               const char *root_file, const char *target,
                               const IREffectResults *effects,
                               IRRuleImage *out, char **error_message) {
  const Type *machine_type = lookup_qualified(checker, "std/rule.Machine");
  const Type *function_type =
      lookup_qualified(checker, "std/rule.MachineFunction");
  const Type *loop_type = lookup_qualified(checker, "std/rule.MachineLoop");
  Image image;
  size_t machine_offset;
  size_t functions_offset;
  size_t count = 0;
  size_t site_capacity = 0;
  size_t total = ir_machine_function_count();
  if (!checker || !out || !error_message) {
    return 0;
  }
  memset(out, 0, sizeof(*out));
  *error_message = NULL;
  if (!machine_type || !function_type || !loop_type) {
    *error_message = strdup("a @rule over the machine needs the Machine, "
                            "MachineFunction and MachineLoop records from "
                            "std/rule");
    return 0;
  }
  for (size_t i = 0; i < total; i++) {
    const IRMachineFunction *fn = ir_machine_function_at(i);
    if (fn && !machine_name_is_rule(program, fn->name)) {
      count++;
    }
  }
  memset(&image, 0, sizeof(image));
  machine_offset = image_reserve(&image, machine_type->size,
                                 machine_type->alignment);
  functions_offset =
      count ? image_reserve(&image, function_type->size * count,
                            function_type->alignment)
            : 0;
  for (size_t i = 0, slot = 0; i < total && !image.failed; i++) {
    const IRMachineFunction *fn = ir_machine_function_at(i);
    size_t base;
    size_t loops_offset = 0;
    if (!fn || machine_name_is_rule(program, fn->name)) {
      continue;
    }
    base = functions_offset + slot * function_type->size;
    slot++;
    image_put_string(&image, base + field_offset(function_type, "name", &image),
                     fn->name);
    image_put_string(&image, base + field_offset(function_type, "file", &image),
                     fn->file ? fn->file : root_file);
    image_put_u64(&image, base + field_offset(function_type, "line", &image),
                  (unsigned long long)fn->line);
    image_put_u64(&image, base + field_offset(function_type, "column", &image),
                  (unsigned long long)fn->column);
    image_put_u64(&image,
                  base + field_offset(function_type, "frame_bytes", &image),
                  (unsigned long long)fn->frame_bytes);
    image_put_u64(&image, base + field_offset(function_type, "spills", &image),
                  (unsigned long long)fn->spills);
    image_put_u64(&image,
                  base + field_offset(function_type, "instructions", &image),
                  (unsigned long long)fn->instructions);
    image_put_bool(
        &image, base + field_offset(function_type, "register_allocated",
                                    &image),
        fn->register_allocated);
    image_put_u64(&image,
                  base + field_offset(function_type, "inlined_calls", &image),
                  (unsigned long long)fn->inlined_calls);
    image_put_u64(&image,
                  base + field_offset(function_type, "calls_left", &image),
                  (unsigned long long)fn->calls_left);
    if (fn->loop_count > 0) {
      loops_offset = image_reserve(&image, loop_type->size * fn->loop_count,
                                   loop_type->alignment);
      for (size_t l = 0; l < fn->loop_count; l++) {
        size_t lb = loops_offset + l * loop_type->size;
        image_put_u64(&image, lb + field_offset(loop_type, "line", &image),
                      (unsigned long long)fn->loops[l].line);
        image_put_u64(&image, lb + field_offset(loop_type, "column", &image),
                      (unsigned long long)fn->loops[l].column);
        image_put_bool(&image,
                       lb + field_offset(loop_type, "vectorized", &image),
                       fn->loops[l].vectorized);
        image_put_string(&image, lb + field_offset(loop_type, "kind", &image),
                         fn->loops[l].kind ? fn->loops[l].kind : "");
      }
    }
    put_slice(&image, base + field_offset(function_type, "loops", &image),
              loops_offset, fn->loop_count);
    {
      const char **performs = NULL;
      size_t perform_count = 0;
      const char **needs = NULL;
      size_t need_count = 0;
      size_t offsets = 0;
      if (effects && ir_effect_results_lookup(effects, fn->name, &performs,
                                              &perform_count, &needs,
                                              &need_count)) {
        offsets = put_string_array(&image, performs, perform_count);
      } else {
        perform_count = 0;
      }
      put_slice(&image, base + field_offset(function_type, "effects", &image),
                offsets, perform_count);
    }
    {
      IRRuleSite site;
      site.file = fn->file ? fn->file : root_file;
      site.line = fn->line;
      site.column = fn->column;
      if (!add_site(out, &site_capacity, site)) {
        image.failed = 1;
      }
      for (size_t l = 0; l < fn->loop_count && !image.failed; l++) {
        site.line = fn->loops[l].line;
        site.column = fn->loops[l].column;
        if (!add_site(out, &site_capacity, site)) {
          image.failed = 1;
        }
      }
    }
  }
  put_slice(&image, machine_offset + field_offset(machine_type, "functions",
                                                  &image),
            functions_offset, count);
  image_put_string(&image,
                   machine_offset + field_offset(machine_type, "target",
                                                 &image),
                   target ? target : "");
  image_put_string(&image,
                   machine_offset + field_offset(machine_type, "file", &image),
                   root_file ? root_file : "");
  {
    size_t pool_base = image.size;
    unsigned char *joined = NULL;
    if (image.pool_size > 0) {
      size_t total = image.size + image.pool_size;
      joined = realloc(image.bytes, total ? total : 1);
      if (!joined) {
        image.failed = 1;
      } else {
        memcpy(joined + image.size, image.pool, image.pool_size);
        image.bytes = joined;
        image.size = total;
      }
    }
    for (size_t i = 0; i < image.string_count && !image.failed; i++) {
      image_put_pointer(&image, image.string_fields[i],
                        pool_base + image.string_offsets[i]);
    }
  }
  free(image.pool);
  free(image.string_fields);
  free(image.string_offsets);
  if (image.failed) {
    free(image.bytes);
    free(image.pointers);
    free(out->sites);
    memset(out, 0, sizeof(*out));
    *error_message = strdup("could not lay out the machine the rules read");
    return 0;
  }
  out->bytes = image.bytes;
  out->size = image.size;
  out->pointer_offsets = image.pointers;
  out->pointer_count = image.pointer_count;
  out->function_count = count;
  out->type_count = 0;
  return 1;
}

static void dump_program(Reflect *reflect) {
  fprintf(stderr, "rule program: %zu functions, %zu types, %zu modules\n",
          reflect->function_count, reflect->type_count,
          reflect->module_count);
  for (size_t i = 0; i < reflect->function_count; i++) {
    RFunction *fn = &reflect->functions[i];
    fprintf(stderr, "  fn %s [%s:%zu]%s%s%s%s ->", fn->qualified,
            fn->site.file ? fn->site.file : "?", fn->site.line,
            fn->is_recursive ? " recursive" : "",
            fn->is_address_taken ? " address-taken" : "",
            fn->has_indirect_calls ? " indirect-calls" : "",
            fn->is_extern ? " extern" : "");
    for (size_t c = 0; c < fn->callee_count; c++) {
      fprintf(stderr, " %s", fn->callees[c]);
    }
    for (size_t m = 0; m < fn->match_count; m++) {
      fprintf(stderr, " matches:%s", fn->matches[m]);
    }
    fprintf(stderr, "\n");
  }
  for (size_t i = 0; i < reflect->type_count; i++) {
    RType *entry = &reflect->types[i];
    fprintf(stderr, "  %s %s%s%s size %lld align %lld layout %lld\n",
            entry->kind, entry->qualified, entry->base[0] ? " of " : "",
            entry->base, entry->size, entry->align, entry->layout);
  }
}

static void reflect_free(Reflect *reflect) {
  for (size_t i = 0; i < reflect->function_count; i++) {
    free(reflect->functions[i].qualified);
    free(reflect->functions[i].callees);
    free(reflect->functions[i].matches);
    free(reflect->functions[i].allocations);
    free(reflect->functions[i].frees);
    free(reflect->functions[i].writes_globals);
  }
  for (size_t i = 0; i < reflect->global_count; i++) {
    free(reflect->globals[i].written_by);
  }
  free(reflect->globals);
  for (size_t i = 0; i < reflect->owned_count; i++) {
    free(reflect->owned[i]);
  }
  free(reflect->owned);
  for (size_t i = 0; i < reflect->type_count; i++) {
    free(reflect->types[i].qualified);
  }
  free(reflect->functions);
  free(reflect->types);
  free(reflect->modules);
}

static void attach_effects(Reflect *reflect, const IREffectResults *effects) {
  if (!effects) {
    return;
  }
  for (size_t i = 0; i < reflect->function_count; i++) {
    RFunction *fn = &reflect->functions[i];
    const char **performs = NULL;
    size_t perform_count = 0;
    const char **needs = NULL;
    size_t need_count = 0;
    if (ir_effect_results_lookup(effects, fn->declared_name, &performs,
                                 &perform_count, &needs, &need_count)) {
      fn->effects = performs;
      fn->effect_count = perform_count;
      fn->requires = needs;
      fn->require_count = need_count;
    }
  }
}

int rule_reflect_build(TypeChecker *checker, ASTNode *program,
                       const char *root_file, const char *target,
                       const IREffectResults *effects, IRRuleImage *out,
                       char **error_message) {
  if (!checker || !program || program->type != AST_PROGRAM || !out ||
      !error_message) {
    return 0;
  }
  memset(out, 0, sizeof(*out));
  *error_message = NULL;
  Program *prog = (Program *)program->data;
  if (!prog) {
    return 0;
  }
  Reflect reflect;
  memset(&reflect, 0, sizeof(reflect));
  reflect.checker = checker;
  int ok = 1;
  for (size_t i = 0; i < prog->declaration_count && ok; i++) {
    ASTNode *decl = prog->declarations[i];
    if (!decl) {
      continue;
    }
    if (decl->type == AST_FUNCTION_DECLARATION) {
      ok = collect_function(&reflect, decl);
    } else if (decl->type == AST_VAR_DECLARATION) {
      ok = collect_global(&reflect, decl);
    } else if (decl->type == AST_STRUCT_DECLARATION ||
               decl->type == AST_ENUM_DECLARATION ||
               decl->type == AST_TYPE_DECLARATION) {
      ok = collect_type(&reflect, decl);
    }
  }
  for (size_t i = 0; i < reflect.function_count && ok; i++) {
    RFunction *fn = &reflect.functions[i];
    ASTNode *decl = NULL;
    for (size_t d = 0; d < prog->declaration_count; d++) {
      ASTNode *candidate = prog->declarations[d];
      if (candidate && candidate->type == AST_FUNCTION_DECLARATION &&
          candidate->data &&
          ((FunctionDeclaration *)candidate->data)->name == fn->declared_name) {
        decl = candidate;
        break;
      }
    }
    if (!decl) {
      continue;
    }
    reflect.current = fn;
    ok = walk_body(&reflect, ((FunctionDeclaration *)decl->data)->body);
  }
  if (ok) {
    ok = mark_recursion(&reflect);
  }
  if (ok) {
    attach_effects(&reflect, effects);
  }
  if (ok && getenv("METTLE_DUMP_RULE_PROGRAM")) {
    dump_program(&reflect);
  }
  if (ok) {
    ok = build_image(&reflect, root_file, target, out, error_message);
  } else if (!*error_message) {
    *error_message = strdup("out of memory while reflecting the program");
  }
  reflect_free(&reflect);
  return ok;
}
