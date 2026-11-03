#include "meta_stage.h"

#include "../compiler/compiler_context.h"
#include "../ir/ir_interp.h"
#include "../ir/ir_lowering.h"
#include "../ir/ir_rules.h"
#include "../lexer/lexer.h"
#include "../parser/parser.h"
#include "rule_reflect.h"
#include "symbol_table.h"
#include "type_checker_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define META_DEFAULT_FUEL 2000000000LL
#define META_MAX_ROUNDS 16

IRFunction *ir_program_find_function(IRProgram *program, const char *name);

typedef struct {
  char **names;
  int *via;
  size_t count;
  size_t capacity;
  int *slots;
  size_t slot_count;
} NameSet;

typedef struct {
  ASTNode *program;
  ErrorReporter *reporter;
  const char *root_file;
  const char *source;
  const char *target;
  long long fuel;
  MetaStageReport *report;
} MetaStage;

typedef struct {
  ASTNode *clone;
  ErrorReporter *reporter;
  SymbolTable *symbols;
  TypeChecker *checker;
  IRProgram *ir;
  const char *first_error;
} StageBuild;

static const char *meta_strip_import(const char *name) {
  return rule_reflect_strip_import_prefix(name);
}

static size_t name_hash(const char *text) {
  size_t hash = 14695981039346656037ull;
  for (const char *p = text; *p; p++) {
    hash ^= (unsigned char)*p;
    hash *= 1099511628211ull;
  }
  return hash;
}

static int name_set_find(const NameSet *set, const char *name) {
  if (!set->slot_count) {
    return -1;
  }
  size_t at = name_hash(name) & (set->slot_count - 1);
  while (set->slots[at] >= 0) {
    if (strcmp(set->names[set->slots[at]], name) == 0) {
      return set->slots[at];
    }
    at = (at + 1) & (set->slot_count - 1);
  }
  return -1;
}

static int name_set_grow(NameSet *set) {
  size_t next = set->slot_count ? set->slot_count * 2 : 256;
  int *slots = malloc(next * sizeof(int));
  if (!slots) {
    return 0;
  }
  for (size_t i = 0; i < next; i++) {
    slots[i] = -1;
  }
  for (size_t i = 0; i < set->count; i++) {
    size_t at = name_hash(set->names[i]) & (next - 1);
    while (slots[at] >= 0) {
      at = (at + 1) & (next - 1);
    }
    slots[at] = (int)i;
  }
  free(set->slots);
  set->slots = slots;
  set->slot_count = next;
  return 1;
}

static int name_set_add(NameSet *set, const char *name, int via) {
  if (!name || !*name || name_set_find(set, name) >= 0) {
    return 1;
  }
  if ((set->count + 1) * 2 >= set->slot_count && !name_set_grow(set)) {
    return 0;
  }
  if (set->count == set->capacity) {
    size_t next = set->capacity ? set->capacity * 2 : 128;
    char **names = realloc(set->names, next * sizeof(char *));
    int *vias = realloc(set->via, next * sizeof(int));
    if (!names || !vias) {
      return 0;
    }
    set->names = names;
    set->via = vias;
    set->capacity = next;
  }
  set->names[set->count] = strdup(name);
  set->via[set->count] = via;
  size_t at = name_hash(name) & (set->slot_count - 1);
  while (set->slots[at] >= 0) {
    at = (at + 1) & (set->slot_count - 1);
  }
  set->slots[at] = (int)set->count;
  set->count++;
  return 1;
}

static void name_set_free(NameSet *set) {
  for (size_t i = 0; i < set->count; i++) {
    free(set->names[i]);
  }
  free(set->names);
  free(set->via);
  free(set->slots);
  memset(set, 0, sizeof(*set));
}

static int word_char(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '_';
}

static void type_words_add(NameSet *types, const char *text, int via) {
  char word[256];
  size_t length = 0;
  if (!text) {
    return;
  }
  for (const char *p = text;; p++) {
    if (word_char(*p) && length + 1 < sizeof(word)) {
      word[length++] = *p;
      continue;
    }
    if (length) {
      word[length] = '\0';
      name_set_add(types, word, via);
      length = 0;
    }
    if (!*p) {
      break;
    }
  }
}

static void collect_names(const ASTNode *node, NameSet *names, NameSet *types,
                          int via) {
  if (!node) {
    return;
  }
  switch (node->type) {
  case AST_IDENTIFIER:
    if (node->data) {
      name_set_add(names, ((const Identifier *)node->data)->name, via);
    }
    break;
  case AST_FUNCTION_CALL:
    if (node->data) {
      name_set_add(names, ((const CallExpression *)node->data)->function_name,
                   via);
    }
    break;
  case AST_MEMBER_ACCESS:
    if (node->data) {
      name_set_add(names, ((const MemberAccess *)node->data)->member, via);
    }
    break;
  case AST_VAR_DECLARATION:
    if (node->data) {
      type_words_add(types, ((const VarDeclaration *)node->data)->type_name,
                     via);
    }
    break;
  case AST_CAST_EXPRESSION:
    if (node->data) {
      type_words_add(types, ((const CastExpression *)node->data)->type_name,
                     via);
    }
    break;
  case AST_NEW_EXPRESSION:
    if (node->data) {
      type_words_add(types, ((const NewExpression *)node->data)->type_name,
                     via);
    }
    break;
  default:
    break;
  }
  for (size_t i = 0; i < node->child_count; i++) {
    collect_names(node->children[i], names, types, via);
  }
}

static void collect_function_names(const FunctionDeclaration *fd,
                                   NameSet *names, NameSet *types, int via) {
  type_words_add(types, fd->return_type, via);
  for (size_t i = 0; i < fd->parameter_count; i++) {
    type_words_add(types, fd->parameter_types[i], via);
  }
  collect_names(fd->body, names, types, via);
}

static const FunctionDeclaration *find_function(const Program *program,
                                                const char *name, int role,
                                                const ASTNode **out_decl) {
  for (size_t i = 0; i < program->declaration_count; i++) {
    const ASTNode *decl = program->declarations[i];
    const FunctionDeclaration *fd;
    if (!decl || decl->type != AST_FUNCTION_DECLARATION || !decl->data) {
      continue;
    }
    fd = (const FunctionDeclaration *)decl->data;
    if (fd->name && fd->meta_role == role &&
        strcmp(meta_strip_import(fd->name), name) == 0) {
      if (out_decl) {
        *out_decl = decl;
      }
      return fd;
    }
  }
  return NULL;
}

static const FunctionDeclaration *find_any_metafunction(const Program *program,
                                                        const char *name) {
  const FunctionDeclaration *fd =
      find_function(program, name, META_ROLE_LAYOUT, NULL);
  return fd ? fd : find_function(program, name, META_ROLE_GENERATE, NULL);
}

static void report_at(MetaStage *stage, SourceLocation location,
                      const char *code, const char *message,
                      const char *note) {
  error_reporter_add_error(stage->reporter, ERROR_SEMANTIC, location, message);
  error_reporter_set_last_code(stage->reporter, code);
  if (note) {
    error_reporter_add_note_of_span(stage->reporter,
                                    source_span_from_location(location, 1),
                                    note);
  }
}

static int struct_pending(const StructDeclaration *sd) {
  return sd->meta_decorator_count > 0 && !sd->meta_layout;
}

static int struct_has_layout_decorator(const Program *program,
                                       const StructDeclaration *sd) {
  for (size_t d = 0; d < sd->meta_decorator_count; d++) {
    if (find_function(program, sd->meta_decorators[d].name, META_ROLE_LAYOUT,
                      NULL)) {
      return 1;
    }
  }
  return 0;
}

static int program_has_decorators(const Program *program) {
  for (size_t i = 0; i < program->declaration_count; i++) {
    const ASTNode *decl = program->declarations[i];
    if (!decl || !decl->data) {
      continue;
    }
    if (decl->type == AST_STRUCT_DECLARATION &&
        ((const StructDeclaration *)decl->data)->meta_decorator_count > 0) {
      return 1;
    }
    if (decl->type == AST_FUNCTION_DECLARATION &&
        ((const FunctionDeclaration *)decl->data)->meta_decorator_count > 0) {
      return 1;
    }
    if (decl->type == AST_TRAIT_DECLARATION &&
        ((const TraitDeclaration *)decl->data)->meta_decorator_count > 0) {
      return 1;
    }
  }
  return 0;
}

static int program_declares_generator_calls(const Program *program) {
  for (size_t i = 0; i < program->declaration_count; i++) {
    const ASTNode *decl = program->declarations[i];
    if (decl && decl->type == AST_FUNCTION_DECLARATION && decl->data &&
        ((const FunctionDeclaration *)decl->data)->meta_role ==
            META_ROLE_GENERATE) {
      return 1;
    }
  }
  return 0;
}

static int check_one_decorator_known(MetaStage *stage, const Program *program,
                                     const MetaDecorator *decorator) {
  char message[400];
  if (find_any_metafunction(program, decorator->name)) {
    return 1;
  }
  snprintf(message, sizeof(message),
           "Unknown decorator after '@': no '@layout' or '@generate' function "
           "is named '%s', and it is not one of 'inline', 'noinline', 'pure', "
           "'noalloc', 'test', 'rule', 'naked', 'interrupt', 'swappable', "
           "'simd', 'parallel', 'numerics' or 'unroll'",
           decorator->name);
  report_at(stage, decorator->location, "L0001", message, NULL);
  return 0;
}

static int check_decorators_known(MetaStage *stage, const Program *program) {
  int ok = 1;
  for (size_t i = 0; i < program->declaration_count; i++) {
    const ASTNode *decl = program->declarations[i];
    const MetaDecorator *decorators = NULL;
    size_t count = 0;
    if (!decl || !decl->data) {
      continue;
    }
    if (decl->type == AST_STRUCT_DECLARATION) {
      decorators = ((const StructDeclaration *)decl->data)->meta_decorators;
      count = ((const StructDeclaration *)decl->data)->meta_decorator_count;
    } else if (decl->type == AST_FUNCTION_DECLARATION) {
      decorators = ((const FunctionDeclaration *)decl->data)->meta_decorators;
      count = ((const FunctionDeclaration *)decl->data)->meta_decorator_count;
    } else if (decl->type == AST_TRAIT_DECLARATION) {
      decorators = ((const TraitDeclaration *)decl->data)->meta_decorators;
      count = ((const TraitDeclaration *)decl->data)->meta_decorator_count;
    }
    for (size_t d = 0; d < count; d++) {
      if (!check_one_decorator_known(stage, program, &decorators[d])) {
        ok = 0;
      }
    }
  }
  return ok;
}

static void close_over_functions(const Program *clone, NameSet *names,
                                 NameSet *types) {
  size_t done = 0;
  while (done < names->count) {
    const char *name = names->names[done];
    for (size_t i = 0; i < clone->declaration_count; i++) {
      const ASTNode *decl = clone->declarations[i];
      const FunctionDeclaration *fd;
      if (!decl || decl->type != AST_FUNCTION_DECLARATION || !decl->data) {
        continue;
      }
      fd = (const FunctionDeclaration *)decl->data;
      if (fd->name && strcmp(fd->name, name) == 0) {
        collect_function_names(fd, names, types, (int)done);
      }
    }
    done++;
  }
}

static void prune_clone(Program *clone, const NameSet *keep) {
  size_t kept = 0;
  for (size_t i = 0; i < clone->declaration_count; i++) {
    ASTNode *decl = clone->declarations[i];
    int retain = 1;
    if (decl && decl->type == AST_FUNCTION_DECLARATION && decl->data) {
      FunctionDeclaration *fd = (FunctionDeclaration *)decl->data;
      retain = fd->is_test || fd->is_rule || fd->rewrite_role
                   ? 0
                   : (!fd->body || (fd->name && name_set_find(keep, fd->name) >= 0));
    }
    if (retain) {
      clone->declarations[kept++] = decl;
    }
  }
  clone->declaration_count = kept;
}

static void clear_clone_roles(Program *clone) {
  size_t kept = 0;
  for (size_t i = 0; i < clone->declaration_count; i++) {
    ASTNode *decl = clone->declarations[i];
    if (!decl || decl->type != AST_TRAIT_DECLARATION) {
      clone->declarations[kept++] = decl;
    }
  }
  clone->declaration_count = kept;
  for (size_t i = 0; i < clone->declaration_count; i++) {
    ASTNode *decl = clone->declarations[i];
    if (!decl || !decl->data) {
      continue;
    }
    if (decl->type == AST_FUNCTION_DECLARATION) {
      FunctionDeclaration *fd = (FunctionDeclaration *)decl->data;
      fd->meta_role = META_ROLE_NONE;
      fd->meta_decorator_count = 0;
    } else if (decl->type == AST_STRUCT_DECLARATION) {
      ((StructDeclaration *)decl->data)->meta_decorator_count = 0;
    } else if (decl->type == AST_TRAIT_DECLARATION) {
      ((TraitDeclaration *)decl->data)->meta_decorator_count = 0;
    }
  }
}

static int append_parsed(MetaStage *stage, ASTNode *into, const char *text,
                         const char *virtual_file, ASTNode ***out_new,
                         size_t *out_count) {
  Program *program = (Program *)into->data;
  Lexer *lexer = lexer_create(text);
  ErrorReporter *reporter = error_reporter_create(virtual_file, text);
  Parser *parser;
  ASTNode *parsed;
  Program *data;
  const char *previous = mettle_compiler_ctx()->current_filename;
  if (!lexer || !reporter) {
    return 0;
  }
  parser = parser_create_with_error_reporter(lexer, reporter);
  if (!parser) {
    return 0;
  }
  mettle_compiler_ctx_set_current_filename(virtual_file);
  parsed = parser_parse_program(parser);
  mettle_compiler_ctx_set_current_filename(previous);
  if (!parsed || error_reporter_has_errors(reporter) || !parsed->data) {
    if (reporter->count > 0 && reporter->errors[0].message && stage->report) {
      stage->report->pending_error = strdup(reporter->errors[0].message);
    }
    return 0;
  }
  data = (Program *)parsed->data;
  ASTNode **grown =
      realloc(program->declarations, (program->declaration_count +
                                      data->declaration_count + 1) *
                                         sizeof(ASTNode *));
  if (!grown) {
    return 0;
  }
  program->declarations = grown;
  size_t first = program->declaration_count;
  for (size_t i = 0; i < data->declaration_count; i++) {
    program->declarations[program->declaration_count++] = data->declarations[i];
    ast_add_child(into, data->declarations[i]);
  }
  if (out_new) {
    *out_new = program->declarations + first;
  }
  if (out_count) {
    *out_count = data->declaration_count;
  }
  return 1;
}

static const FunctionDeclaration *library_function(const Program *program,
                                                   const char *name) {
  return find_function(program, name, META_ROLE_NONE, NULL);
}

static char *layout_driver_text(const Program *program,
                                const StructDeclaration *sd, size_t target,
                                char *why, size_t why_size) {
  const FunctionDeclaration *find_type =
      library_function(program, "layout_type");
  const FunctionDeclaration *make_default =
      library_function(program, "layout_default");
  size_t capacity = 4096;
  size_t used = 0;
  char *text;
  if (!find_type || !make_default || find_type->parameter_count != 2 ||
      make_default->parameter_count != 1) {
    snprintf(why, why_size, "a layout decorator needs 'import \"std/layout\";'");
    return NULL;
  }
  text = malloc(capacity);
  if (!text) {
    return NULL;
  }
  used += (size_t)snprintf(
      text + used, capacity - used,
      "fn __meta_layout_%zu(p: %s) -> %s {\n"
      "  var t: %s = %s(p, \"%s\");\n"
      "  var l: %s = %s(t);\n",
      target, find_type->parameter_types[0], make_default->return_type,
      find_type->return_type, find_type->name, meta_strip_import(sd->name),
      make_default->return_type, make_default->name);
  for (size_t d = 0; d < sd->meta_decorator_count; d++) {
    const MetaDecorator *decorator = &sd->meta_decorators[d];
    const FunctionDeclaration *fn =
        find_function(program, decorator->name, META_ROLE_LAYOUT, NULL);
    if (!fn) {
      continue;
    }
    if (used + 1024 > capacity) {
      capacity *= 2;
      char *grown = realloc(text, capacity);
      if (!grown) {
        free(text);
        return NULL;
      }
      text = grown;
    }
    if (fn->parameter_count == 3) {
      used += (size_t)snprintf(text + used, capacity - used,
                               "  var a%zu: string[] = new string[%zu];\n", d,
                               decorator->arg_count);
      for (size_t a = 0; a < decorator->arg_count; a++) {
        used += (size_t)snprintf(text + used, capacity - used,
                                 "  a%zu[%zu] = \"%s\";\n", d, a,
                                 decorator->args[a]);
      }
      used += (size_t)snprintf(text + used, capacity - used,
                               "  l = %s(t, l, a%zu);\n", fn->name, d);
    } else if (fn->parameter_count == 2) {
      if (decorator->arg_count > 0) {
        snprintf(why, why_size,
                 "'%s' takes no arguments: declare a third parameter "
                 "'args: string[]' to read them",
                 decorator->name);
        free(text);
        return NULL;
      }
      used += (size_t)snprintf(text + used, capacity - used,
                               "  l = %s(t, l);\n", fn->name);
    } else {
      snprintf(why, why_size,
               "a @layout function is 'fn %s(t: TypeInfo, l: Layout) -> Layout', "
               "with an optional third parameter 'args: string[]'",
               decorator->name);
      free(text);
      return NULL;
    }
  }
  snprintf(text + used, capacity - used, "  return l;\n}\n");
  return text;
}

static int stage_build(MetaStage *stage, const NameSet *keep, const char *drivers,
                       StageBuild *out) {
  char *lower_error = NULL;
  memset(out, 0, sizeof(*out));
  out->clone = ast_clone_node(stage->program);
  if (!out->clone) {
    return 0;
  }
  Program *clone = (Program *)out->clone->data;
  clear_clone_roles(clone);
  if (keep) {
    prune_clone(clone, keep);
  }
  if (drivers && !append_parsed(stage, out->clone, drivers, "<metafunctions>",
                                NULL, NULL)) {
    return 0;
  }
  out->reporter = error_reporter_create(stage->root_file, stage->source);
  out->symbols = symbol_table_create();
  if (!out->reporter || !out->symbols) {
    return 0;
  }
  out->checker =
      type_checker_create_with_error_reporter(out->symbols, out->reporter);
  if (!out->checker) {
    return 0;
  }
  stage->report->stage_builds++;
  type_checker_reset_declared_type_templates();
  int checked = type_checker_check_program(out->checker, out->clone);
  type_checker_reset_declared_type_templates();
  if (!checked) {
    out->first_error = out->reporter->count > 0 ? out->reporter->errors[0].message
                       : out->checker->error_message
                           ? out->checker->error_message
                           : "the metaprogram did not type check";
    return 0;
  }
  out->ir = ir_lower_program(out->clone, out->checker, out->symbols,
                             &lower_error, 0, 0);
  if (!out->ir) {
    out->first_error = lower_error ? lower_error : "the metaprogram did not lower";
    return 0;
  }
  return 1;
}

static int read_bytes(IRInterpMachine *machine, unsigned long long address,
                      void *out, size_t size) {
  return ir_interp_read_bytes(machine, address, (unsigned char *)out,
                              (long long)size) == (long long)size;
}

static long long read_i64(IRInterpMachine *machine, unsigned long long address) {
  long long value = 0;
  read_bytes(machine, address, &value, 8);
  return value;
}

static char *read_text(IRInterpMachine *machine, unsigned long long address) {
  unsigned long long chars = (unsigned long long)read_i64(machine, address);
  long long length = read_i64(machine, address + 8);
  char *text;
  if (length < 0 || length > (1ll << 30)) {
    return NULL;
  }
  text = malloc((size_t)length + 1);
  if (!text) {
    return NULL;
  }
  if (length > 0 && !read_bytes(machine, chars, text, (size_t)length)) {
    free(text);
    return NULL;
  }
  text[length] = '\0';
  return text;
}

static size_t field_at(const Type *type, const char *name) {
  int index = type_get_field_index(type, name);
  return index >= 0 ? type->field_offsets[index] : 0;
}

static const Type *slice_element(const Type *type, const char *field) {
  int index = type_get_field_index(type, field);
  return index >= 0 && type->field_types[index]
             ? type->field_types[index]->base_type
             : NULL;
}

static TypeMetaLayout *decode_layout(IRInterpMachine *machine,
                                     const Type *layout_type,
                                     unsigned long long address,
                                     const char *written_by) {
  const Type *region_type = slice_element(layout_type, "regions");
  const Type *place_type = slice_element(layout_type, "places");
  TypeMetaLayout *layout = calloc(1, sizeof(TypeMetaLayout));
  if (!layout || !region_type || !place_type) {
    return NULL;
  }
  unsigned long long regions = address + field_at(layout_type, "regions");
  unsigned long long places = address + field_at(layout_type, "places");
  unsigned long long region_data = (unsigned long long)read_i64(machine, regions);
  unsigned long long place_data = (unsigned long long)read_i64(machine, places);
  layout->region_count = (size_t)read_i64(machine, regions + 8);
  layout->place_count = (size_t)read_i64(machine, places + 8);
  if (layout->region_count > 4096 || layout->place_count > 65536) {
    return NULL;
  }
  layout->regions = calloc(layout->region_count + 1, sizeof(TypeMetaRegion));
  layout->places = calloc(layout->place_count + 1, sizeof(TypeMetaPlace));
  if (!layout->regions || !layout->places) {
    return NULL;
  }
  for (size_t r = 0; r < layout->region_count; r++) {
    unsigned long long at = region_data + r * region_type->size;
    layout->regions[r].stride =
        (size_t)read_i64(machine, at + field_at(region_type, "stride"));
    layout->regions[r].alignment =
        (size_t)read_i64(machine, at + field_at(region_type, "align"));
  }
  for (size_t p = 0; p < layout->place_count; p++) {
    unsigned long long at = place_data + p * place_type->size;
    TypeMetaPlace *place = &layout->places[p];
    place->field = read_text(machine, at + field_at(place_type, "field"));
    place->region =
        (uint32_t)read_i64(machine, at + field_at(place_type, "region"));
    place->offset =
        (size_t)read_i64(machine, at + field_at(place_type, "offset"));
    place->word = (uint32_t)read_i64(machine, at + field_at(place_type, "word"));
    place->bit = (uint32_t)read_i64(machine, at + field_at(place_type, "bit"));
    place->width =
        (uint32_t)read_i64(machine, at + field_at(place_type, "width"));
    place->scale =
        (uint32_t)read_i64(machine, at + field_at(place_type, "scale"));
  }
  char *allocator =
      read_text(machine, address + field_at(layout_type, "allocator"));
  layout->allocator = allocator && *allocator ? allocator : NULL;
  char *releaser =
      read_text(machine, address + field_at(layout_type, "releaser"));
  layout->releaser = releaser && *releaser ? releaser : NULL;
  layout->written_by = written_by;
  return layout;
}

static void refresh_type(Type *type) {
  if (!type) {
    return;
  }
  if (type->kind == TYPE_ARRAY) {
    refresh_type(type->base_type);
    type_compute_layout(type);
    return;
  }
  if (type->kind != TYPE_STRUCT || type->meta_layout) {
    return;
  }
  for (size_t i = 0; i < type->field_count; i++) {
    refresh_type(type->field_types[i]);
  }
  type_compute_layout(type);
}

static void refresh_stage_types(TypeChecker *checker) {
  for (size_t i = 0; i < checker->type_table_count; i++) {
    refresh_type(checker->type_table[i]);
  }
}

static const char *interp_failure(IRInterpMachine *machine,
                                  IRInterpStatus status) {
  if (status == IR_INTERP_FUEL) {
    return "did not finish within its step budget";
  }
  if (status == IR_INTERP_UNSUPPORTED) {
    return ir_interp_status_detail(machine);
  }
  return ir_interp_status_detail(machine);
}

static int run_driver(MetaStage *stage, StageBuild *build, const char *driver,
                      IRRuleImage *image, IRInterpMachine **out_machine,
                      unsigned long long *out_address, SourceLocation at) {
  char message[800];
  IRFunction *function = ir_program_find_function(build->ir, driver);
  IRInterpMachine *machine = ir_interp_create(build->ir);
  unsigned long long address = 0;
  if (!function || !machine ||
      !ir_rule_image_place(machine, image, &address)) {
    report_at(stage, at, "L0001",
              "the compiler could not prepare the interpreter for this "
              "metafunction",
              NULL);
    return 0;
  }
  IRInterpValue arg;
  IRInterpValue result;
  memset(&arg, 0, sizeof(arg));
  memset(&result, 0, sizeof(result));
  arg.i = (long long)address;
  IRInterpStatus status =
      ir_interp_run(machine, function, &arg, 1, &result, stage->fuel);
  stage->report->steps += stage->fuel - ir_interp_fuel_remaining(machine);
  if (status != IR_INTERP_OK) {
    snprintf(message, sizeof(message), "the metafunction %s",
             interp_failure(machine, status));
    report_at(stage, at, "L0001", message, NULL);
    ir_interp_destroy(machine);
    return 0;
  }
  *out_machine = machine;
  *out_address = (unsigned long long)result.i;
  return 1;
}

typedef struct {
  ASTNode *decl;
  StructDeclaration *sd;
  int done;
} LayoutTarget;

static int holds_target_by_value(const StructDeclaration *holder,
                                 const StructDeclaration *held) {
  size_t length = strlen(held->name);
  for (size_t f = 0; f < holder->field_count; f++) {
    const char *type = holder->field_types[f];
    if (type && strncmp(type, held->name, length) == 0 &&
        (type[length] == '\0' || type[length] == '[')) {
      return 1;
    }
  }
  return 0;
}

static int print_chain(const NameSet *names, const NameSet *types, int entry,
                       char *out, size_t capacity) {
  int at = types->via[entry];
  size_t used = (size_t)snprintf(out, capacity, "%s", types->names[entry]);
  while (at >= 0 && used + 8 < capacity) {
    used += (size_t)snprintf(out + used, capacity - used, " <- %s",
                             meta_strip_import(names->names[at]));
    at = names->via[at];
  }
  return 1;
}

static int run_layouts(MetaStage *stage, LayoutTarget *targets, size_t count,
                       const NameSet *names, const NameSet *types) {
  Program *program = (Program *)stage->program->data;
  char why[800];
  size_t capacity = 4096 + count * 2048;
  char *drivers = malloc(capacity);
  size_t used = 0;
  StageBuild build;
  if (!drivers) {
    return 0;
  }
  drivers[0] = '\0';
  for (size_t i = 0; i < count; i++) {
    int entry = name_set_find(types, targets[i].sd->name);
    if (entry >= 0) {
      char chain[600];
      char message[800];
      print_chain(names, types, entry, chain, sizeof(chain));
      snprintf(message, sizeof(message),
               "A metafunction reaches '%s', whose layout it decides: %s",
               targets[i].sd->name, chain);
      report_at(stage, targets[i].decl->location, "L0006", message,
                "use field names and sizes from the TypeInfo the metafunction "
                "receives");
      free(drivers);
      return 0;
    }
    char *text = layout_driver_text(program, targets[i].sd, i, why, sizeof(why));
    if (!text) {
      report_at(stage, targets[i].decl->location, "L0001", why, NULL);
      free(drivers);
      return 0;
    }
    size_t length = strlen(text);
    if (used + length + 1 > capacity) {
      capacity = (used + length + 1) * 2;
      char *grown = realloc(drivers, capacity);
      if (!grown) {
        free(text);
        return 0;
      }
      drivers = grown;
    }
    memcpy(drivers + used, text, length + 1);
    used += length;
    free(text);
  }
  NameSet keep = {0};
  for (size_t i = 0; i < names->count; i++) {
    name_set_add(&keep, names->names[i], -1);
  }
  char driver_name[64];
  for (size_t i = 0; i < count; i++) {
    snprintf(driver_name, sizeof(driver_name), "__meta_layout_%zu", i);
    name_set_add(&keep, driver_name, -1);
  }
  int built = stage_build(stage, &keep, drivers, &build);
  name_set_free(&keep);
  if (!built) {
    stage->report->stage_builds--;
    const char *pruned_error = build.first_error;
    built = stage_build(stage, NULL, drivers, &build);
    if (!built) {
      free(drivers);
      stage->report->pending_error =
          build.first_error ? build.first_error : pruned_error;
      return 1;
    }
  }
  free(drivers);

  const FunctionDeclaration *make_default =
      library_function(program, "layout_default");
  Type *layout_type =
      type_checker_get_type_by_name(build.checker, make_default->return_type);
  size_t remaining = count;
  while (remaining > 0) {
    size_t progressed = 0;
    for (size_t i = 0; i < count; i++) {
      int waiting = 0;
      if (targets[i].done) {
        continue;
      }
      for (size_t j = 0; j < count && !waiting; j++) {
        if (j != i && !targets[j].done &&
            holds_target_by_value(targets[i].sd, targets[j].sd)) {
          waiting = 1;
        }
      }
      if (waiting) {
        continue;
      }
      IRRuleImage image;
      char *image_error = NULL;
      if (!rule_reflect_build(build.checker, build.clone, stage->root_file,
                              stage->target, NULL, &image, &image_error)) {
        report_at(stage, targets[i].decl->location, "L0001",
                  image_error ? image_error
                              : "could not reflect the program for this layout",
                  NULL);
        return 0;
      }
      IRInterpMachine *machine = NULL;
      unsigned long long address = 0;
      snprintf(driver_name, sizeof(driver_name), "__meta_layout_%zu", i);
      if (!run_driver(stage, &build, driver_name, &image, &machine, &address,
                      targets[i].decl->location)) {
        return 0;
      }
      char *written_by = strdup(targets[i].sd->meta_decorators[0].name);
      TypeMetaLayout *layout =
          decode_layout(machine, layout_type, address, written_by);
      ir_interp_destroy(machine);
      ir_rule_image_free(&image);
      if (!layout) {
        report_at(stage, targets[i].decl->location, "L0001",
                  "the layout function returned a Layout the compiler could "
                  "not read",
                  NULL);
        return 0;
      }
      Type *stage_type =
          type_checker_get_type_by_name(build.checker, targets[i].sd->name);
      if (!type_apply_meta_layout(stage_type, layout, why, sizeof(why))) {
        char message[900];
        snprintf(message, sizeof(message), "Layout from '@%s' on '%s' fails: %s",
                 written_by, targets[i].sd->name, why);
        report_at(stage, targets[i].decl->location, "L0002", message, NULL);
        return 0;
      }
      refresh_stage_types(build.checker);
      targets[i].sd->meta_layout = layout;
      targets[i].done = 1;
      stage->report->layouts++;
      remaining--;
      progressed++;
    }
    if (!progressed) {
      report_at(stage, targets[0].decl->location, "L0006",
                "These layouts wait on each other by value", NULL);
      return 0;
    }
  }
  return 1;
}

typedef enum {
  GEN_ON_FUNCTION,
  GEN_ON_STRUCT,
  GEN_ON_TRAIT,
  GEN_AT_CALL
} GeneratorSite;

typedef struct {
  GeneratorSite site;
  ASTNode *decl;
  ASTNode *call;
  const MetaDecorator *decorator;
  const FunctionDeclaration *generator;
  const char *generator_name;
  const char *target;
  char *inner_name;
  const char *reuse_name;
} GenWork;

typedef struct {
  GenWork *items;
  size_t count;
} GenList;

static int gen_list_add(GenList *list, GenWork work) {
  GenWork *grown = realloc(list->items, (list->count + 1) * sizeof(GenWork));
  if (!grown) {
    return 0;
  }
  list->items = grown;
  list->items[list->count++] = work;
  return 1;
}

static const FunctionDeclaration *generator_named(const Program *program,
                                                  const char *name) {
  return find_function(program, name, META_ROLE_GENERATE, NULL);
}

static void collect_generator_calls(MetaStage *stage, ASTNode *node, GenList *list) {
  Program *program = (Program *)stage->program->data;
  if (!node) {
    return;
  }
  if (node->type == AST_FUNCTION_CALL && node->data) {
    CallExpression *call = (CallExpression *)node->data;
    const FunctionDeclaration *generator =
        call->function_name ? generator_named(program, call->function_name)
                            : NULL;
    if (generator && call->argument_count >= 1 &&
        call->arguments[0]->type == AST_IDENTIFIER &&
        call->arguments[0]->data) {
      GenWork work;
      memset(&work, 0, sizeof(work));
      work.site = GEN_AT_CALL;
      work.call = node;
      work.generator = generator;
      work.generator_name = call->function_name;
      work.target = ((Identifier *)call->arguments[0]->data)->name;
      gen_list_add(list, work);
      return;
    }
  }
  for (size_t i = 0; i < node->child_count; i++) {
    collect_generator_calls(stage, node->children[i], list);
  }
}

static char *unused_inner_name(const Program *program, const char *name) {
  size_t length = strlen(name) + 32;
  char *inner = malloc(length);
  int suffix = 0;
  if (!inner) {
    return NULL;
  }
  snprintf(inner, length, "%s__inner", name);
  while (find_function(program, inner, META_ROLE_NONE, NULL)) {
    suffix++;
    snprintf(inner, length, "%s__inner%d", name, suffix);
  }
  return inner;
}

static int collect_generators(MetaStage *stage, GenList *list) {
  Program *program = (Program *)stage->program->data;
  for (size_t i = 0; i < program->declaration_count; i++) {
    ASTNode *decl = program->declarations[i];
    if (!decl || !decl->data) {
      continue;
    }
    if (decl->type == AST_FUNCTION_DECLARATION) {
      FunctionDeclaration *fd = (FunctionDeclaration *)decl->data;
      if (fd->meta_decorator_count > 0) {
        const MetaDecorator *last =
            &fd->meta_decorators[fd->meta_decorator_count - 1];
        const FunctionDeclaration *generator =
            generator_named(program, last->name);
        if (!generator) {
          char message[300];
          snprintf(message, sizeof(message),
                   "'@%s' is a layout function, and it decorates the function "
                   "'%s'",
                   last->name, fd->name);
          report_at(stage, last->location, "L0001", message, NULL);
          return 0;
        }
        GenWork work;
        memset(&work, 0, sizeof(work));
        work.site = GEN_ON_FUNCTION;
        work.decl = decl;
        work.decorator = last;
        work.generator = generator;
        work.generator_name = last->name;
        work.target = fd->name;
        work.inner_name = unused_inner_name(program, fd->name);
        if (!gen_list_add(list, work)) {
          return 0;
        }
      }
      if (!fd->meta_role) {
        collect_generator_calls(stage, fd->body, list);
      }
    } else if (decl->type == AST_TRAIT_DECLARATION) {
      TraitDeclaration *td = (TraitDeclaration *)decl->data;
      for (size_t d = 0; d < td->meta_decorator_count; d++) {
        const FunctionDeclaration *generator =
            generator_named(program, td->meta_decorators[d].name);
        if (!generator) {
          char message[300];
          snprintf(message, sizeof(message),
                   "'@%s' is a layout function, and it decorates the trait '%s'",
                   td->meta_decorators[d].name, td->name);
          report_at(stage, td->meta_decorators[d].location, "L0001", message,
                    NULL);
          return 0;
        }
        GenWork work;
        memset(&work, 0, sizeof(work));
        work.site = GEN_ON_TRAIT;
        work.decl = decl;
        work.decorator = &td->meta_decorators[d];
        work.generator = generator;
        work.generator_name = td->meta_decorators[d].name;
        work.target = td->name;
        if (!gen_list_add(list, work)) {
          return 0;
        }
      }
    } else if (decl->type == AST_STRUCT_DECLARATION) {
      StructDeclaration *sd = (StructDeclaration *)decl->data;
      for (size_t d = 0; d < sd->meta_decorator_count; d++) {
        const FunctionDeclaration *generator =
            generator_named(program, sd->meta_decorators[d].name);
        if (generator) {
          GenWork work;
          memset(&work, 0, sizeof(work));
          work.site = GEN_ON_STRUCT;
          work.decl = decl;
          work.decorator = &sd->meta_decorators[d];
          work.generator = generator;
          work.generator_name = sd->meta_decorators[d].name;
          work.target = sd->name;
          if (!gen_list_add(list, work)) {
            return 0;
          }
        }
      }
    }
  }
  return 1;
}

static void text_append(char **text, size_t *used, size_t *capacity,
                        const char *more) {
  size_t length = strlen(more);
  if (*used + length + 1 > *capacity) {
    size_t next = (*used + length + 1) * 2;
    char *grown = realloc(*text, next);
    if (!grown) {
      return;
    }
    *text = grown;
    *capacity = next;
  }
  memcpy(*text + *used, more, length + 1);
  *used += length;
}

static char *generator_driver_text(const Program *program, const GenList *list) {
  char *text = NULL;
  size_t used = 0;
  size_t capacity = 0;
  char line[1024];
  text_append(&text, &used, &capacity, "");
  for (size_t i = 0; i < list->count; i++) {
    const GenWork *work = &list->items[i];
    const FunctionDeclaration *g = work->generator;
    const char *parameter = g->parameter_count > 0 ? g->parameter_types[0] : "";
    if (strcmp(parameter, "FunctionInfo") == 0) {
      snprintf(line, sizeof(line),
               "fn __meta_gen_%zu(f: FunctionInfo) -> Generated {\n", i);
      text_append(&text, &used, &capacity, line);
    } else if (strcmp(parameter, "TypeInfo") == 0) {
      const FunctionDeclaration *find = library_function(program, "layout_type");
      snprintf(line, sizeof(line),
               "fn __meta_gen_%zu(p: Program) -> Generated {\n"
               "  var f: TypeInfo = %s(p, \"%s\");\n",
               i, find ? find->name : "layout_type", work->target);
      text_append(&text, &used, &capacity, line);
    } else if (strcmp(parameter, "TraitInfo") == 0 && work->site == GEN_ON_TRAIT) {
      const TraitDeclaration *td = (const TraitDeclaration *)work->decl->data;
      snprintf(line, sizeof(line),
               "fn __meta_gen_%zu(p: Program) -> Generated {\n"
               "  var f: TraitInfo;\n  f.name = \"%s\";\n"
               "  f.implementors = new string[%zu];\n"
               "  f.methods = new MethodInfo[%zu];\n",
               i, td->name, td->implementor_count, td->method_count);
      text_append(&text, &used, &capacity, line);
      for (size_t k = 0; k < td->implementor_count; k++) {
        snprintf(line, sizeof(line), "  f.implementors[%zu] = \"%s\";\n", k,
                 td->implementors[k]);
        text_append(&text, &used, &capacity, line);
      }
      for (size_t m = 0; m < td->method_count; m++) {
        const FunctionDeclaration *method =
            (const FunctionDeclaration *)td->methods[m]->data;
        snprintf(line, sizeof(line),
                 "  f.methods[%zu].name = \"%s\";\n"
                 "  f.methods[%zu].return_type = \"%s\";\n"
                 "  f.methods[%zu].param_names = new string[%zu];\n"
                 "  f.methods[%zu].param_types = new string[%zu];\n",
                 m, method->name, m,
                 method->return_type ? method->return_type : "void", m,
                 method->parameter_count, m, method->parameter_count);
        text_append(&text, &used, &capacity, line);
        for (size_t a = 0; a < method->parameter_count; a++) {
          snprintf(line, sizeof(line),
                   "  f.methods[%zu].param_names[%zu] = \"%s\";\n"
                   "  f.methods[%zu].param_types[%zu] = \"%s\";\n",
                   m, a, method->parameter_names[a], m, a,
                   method->parameter_types[a]);
          text_append(&text, &used, &capacity, line);
        }
      }
    } else {
      return NULL;
    }
    size_t arg_count = work->decorator ? work->decorator->arg_count : 0;
    if (g->parameter_count == 2) {
      snprintf(line, sizeof(line), "  var a: string[] = new string[%zu];\n",
               arg_count);
      text_append(&text, &used, &capacity, line);
      for (size_t a = 0; a < arg_count; a++) {
        snprintf(line, sizeof(line), "  a[%zu] = \"%s\";\n", a,
                 work->decorator->args[a]);
        text_append(&text, &used, &capacity, line);
      }
      snprintf(line, sizeof(line), "  return %s(f, a);\n}\n", g->name);
    } else {
      snprintf(line, sizeof(line), "  return %s(f);\n}\n", g->name);
    }
    text_append(&text, &used, &capacity, line);
  }
  return text;
}

static const StructDeclaration *find_struct_decl(const Program *program,
                                                 const char *name) {
  for (size_t i = 0; i < program->declaration_count; i++) {
    const ASTNode *decl = program->declarations[i];
    if (decl && decl->type == AST_STRUCT_DECLARATION && decl->data &&
        strcmp(((const StructDeclaration *)decl->data)->name, name) == 0) {
      return (const StructDeclaration *)decl->data;
    }
  }
  return NULL;
}

static int apply_generated(MetaStage *stage, GenWork *work, const char *text,
                           const char *name) {
  Program *program = (Program *)stage->program->data;
  char note[300];
  char file[300];
  snprintf(note, sizeof(note), "@%s on %s", work->generator_name, work->target);
  snprintf(file, sizeof(file), "<generated by @%s on %s>", work->generator_name,
           work->target);
  char *kept_note = strdup(note);
  char *kept_file = strdup(file);
  char *kept_text = strdup(text);
  ASTNode **added = NULL;
  size_t added_count = 0;
  const FunctionDeclaration *existing_inner = NULL;
  (void)existing_inner;
  if (work->site == GEN_ON_FUNCTION) {
    FunctionDeclaration *fd = (FunctionDeclaration *)work->decl->data;
    fd->name = work->inner_name;
  }
  if (!append_parsed(stage, stage->program, kept_text, kept_file, &added,
                     &added_count)) {
    char message[900];
    snprintf(message, sizeof(message),
             "'@%s' wrote source that does not parse: %s", work->generator_name,
             stage->report->pending_error ? stage->report->pending_error : "?");
    stage->report->pending_error = NULL;
    SourceLocation at = work->decl ? work->decl->location : work->call->location;
    report_at(stage, at, "L0001", message, kept_text);
    return 0;
  }
  error_reporter_register_source(stage->reporter, kept_file, kept_text);
  program = (Program *)stage->program->data;
  int states_a_relation = 0;
  for (size_t i = 0; i < added_count; i++) {
    ASTNode *decl = added[i];
    if (decl->type == AST_FUNCTION_DECLARATION && decl->data) {
      FunctionDeclaration *fd = (FunctionDeclaration *)decl->data;
      fd->generated_by = kept_note;
      if (fd->is_rule) {
        states_a_relation = 1;
      }
    }
  }
  if (!states_a_relation) {
    SourceLocation at = work->decl ? work->decl->location : work->call->location;
    char message[300];
    snprintf(message, sizeof(message),
             "'@%s' wrote code and no @rule that checks it, so nothing checks "
             "the output",
             work->generator_name);
    error_reporter_add_warning(stage->reporter, ERROR_SEMANTIC, at, message);
    error_reporter_set_last_code(stage->reporter, "L0005");
  }
  if (work->site == GEN_ON_FUNCTION) {
    FunctionDeclaration *inner = (FunctionDeclaration *)work->decl->data;
    MetaDecorator *remaining = inner->meta_decorators;
    size_t remaining_count = inner->meta_decorator_count - 1;
    inner->meta_decorator_count = 0;
    for (size_t i = 0; i < added_count; i++) {
      ASTNode *decl = added[i];
      if (decl->type != AST_FUNCTION_DECLARATION || !decl->data) {
        continue;
      }
      FunctionDeclaration *fd = (FunctionDeclaration *)decl->data;
      if (strcmp(fd->name, work->target) == 0) {
        fd->is_exported = inner->is_exported;
        fd->meta_decorators = remaining;
        fd->meta_decorator_count = remaining_count;
      }
    }
  } else if (work->site == GEN_ON_STRUCT) {
    StructDeclaration *sd = (StructDeclaration *)work->decl->data;
    size_t kept = 0;
    for (size_t d = 0; d < sd->meta_decorator_count; d++) {
      if (&sd->meta_decorators[d] != work->decorator) {
        sd->meta_decorators[kept++] = sd->meta_decorators[d];
      }
    }
    sd->meta_decorator_count = kept;
  } else if (work->site == GEN_ON_TRAIT) {
    TraitDeclaration *td = (TraitDeclaration *)work->decl->data;
    size_t kept = 0;
    for (size_t d = 0; d < td->meta_decorator_count; d++) {
      if (&td->meta_decorators[d] != work->decorator) {
        td->meta_decorators[kept++] = td->meta_decorators[d];
      }
    }
    td->meta_decorator_count = kept;
    if (kept == 0) {
      size_t write = 0;
      for (size_t i = 0; i < program->declaration_count; i++) {
        if (program->declarations[i] != work->decl) {
          program->declarations[write++] = program->declarations[i];
        }
      }
      program->declaration_count = write;
    }
  } else if (work->site == GEN_AT_CALL) {
    ASTNode *call = work->call;
    ASTNode *replacement =
        ast_create_identifier(name && *name ? name : work->target, call->location);
    if (!replacement) {
      return 0;
    }
    call->type = replacement->type;
    call->data = replacement->data;
    call->child_count = 0;
  }
  stage->report->generators++;
  stage->report->generated_bytes += strlen(text);
  return 1;
}

static int run_generators(MetaStage *stage, GenList *list) {
  Program *program = (Program *)stage->program->data;
  NameSet names = {0};
  NameSet types = {0};
  StageBuild build;
  for (size_t i = 0; i < list->count; i++) {
    name_set_add(&names, list->items[i].generator->name, -1);
    name_set_add(&names, list->items[i].target, -1);
  }
  const FunctionDeclaration *helper = library_function(program, "layout_type");
  if (helper) {
    name_set_add(&names, helper->name, -1);
  }
  close_over_functions(program, &names, &types);
  for (size_t i = 0; i < program->declaration_count; i++) {
    const ASTNode *decl = program->declarations[i];
    if (!decl || decl->type == AST_FUNCTION_DECLARATION) {
      continue;
    }
    collect_names(decl, &names, &types, -1);
  }
  close_over_functions(program, &names, &types);
  char *drivers = generator_driver_text(program, list);
  if (!drivers) {
    report_at(stage, list->items[0].generator ? stage->program->location
                                              : stage->program->location,
              "L0001",
              "A @generate function takes a FunctionInfo or a TypeInfo, and "
              "optionally 'args: string[]'",
              NULL);
    return 0;
  }
  int built = stage_build(stage, &names, drivers, &build);
  if (!built) {
    stage->report->stage_builds--;
    const char *pruned_error = build.first_error;
    built = stage_build(stage, NULL, drivers, &build);
    if (!built) {
      stage->report->pending_error =
          build.first_error ? build.first_error : pruned_error;
      free(drivers);
      name_set_free(&names);
      name_set_free(&types);
      return 1;
    }
  }
  free(drivers);
  name_set_free(&names);
  name_set_free(&types);

  for (size_t i = 0; i < list->count; i++) {
    GenWork *work = &list->items[i];
    char driver[64];
    IRRuleImage image;
    char *image_error = NULL;
    IRInterpMachine *machine = NULL;
    unsigned long long address = 0;
    SourceLocation at = work->decl ? work->decl->location : work->call->location;
    int reads_function =
        strcmp(work->generator->parameter_types[0], "FunctionInfo") == 0;
    int imaged =
        reads_function
            ? rule_reflect_build_function(build.checker, build.ir, work->target,
                                          work->inner_name, &image, &image_error)
            : rule_reflect_build(build.checker, build.clone, stage->root_file,
                                 stage->target, NULL, &image, &image_error);
    if (!imaged) {
      report_at(stage, at, "L0001",
                image_error ? image_error : "could not reflect the target",
                NULL);
      return 0;
    }
    snprintf(driver, sizeof(driver), "__meta_gen_%zu", i);
    if (!run_driver(stage, &build, driver, &image, &machine, &address, at)) {
      return 0;
    }
    Type *generated_type =
        type_checker_get_type_by_name(build.checker, "Generated");
    if (!generated_type) {
      report_at(stage, at, "L0001", "a generator needs 'import \"std/meta\";'",
                NULL);
      return 0;
    }
    char *refused =
        read_text(machine, address + field_at(generated_type, "refused"));
    char *text = read_text(machine, address + field_at(generated_type, "text"));
    char *name = read_text(machine, address + field_at(generated_type, "name"));
    ir_interp_destroy(machine);
    ir_rule_image_free(&image);
    if (refused && *refused) {
      report_at(stage, at, "L0001", refused, NULL);
      return 0;
    }
    if (!text) {
      report_at(stage, at, "L0001",
                "the generator returned text the compiler could not read",
                NULL);
      return 0;
    }
    int reused = 0;
    if (work->site == GEN_AT_CALL) {
      for (size_t earlier = 0; earlier < i; earlier++) {
        GenWork *other = &list->items[earlier];
        if (other->site == GEN_AT_CALL && other->reuse_name &&
            other->generator == work->generator &&
            strcmp(other->target, work->target) == 0) {
          ASTNode *replacement =
              ast_create_identifier(other->reuse_name, work->call->location);
          work->call->type = replacement->type;
          work->call->data = replacement->data;
          work->call->child_count = 0;
          work->reuse_name = other->reuse_name;
          reused = 1;
          break;
        }
      }
      if (!reused) {
        work->reuse_name = name && *name ? name : work->target;
      }
    }
    if (!reused && !apply_generated(stage, work, text, name)) {
      return 0;
    }
  }
  return 1;
}

static void direct_generated_calls(const Program *program, ASTNode *node) {
  if (!node) {
    return;
  }
  if (node->type == AST_FUNC_PTR_CALL && node->data) {
    FuncPtrCall *call = (FuncPtrCall *)node->data;
    if (call->function && call->function->type == AST_IDENTIFIER &&
        call->function->data) {
      const char *name = ((Identifier *)call->function->data)->name;
      const FunctionDeclaration *target =
          find_function(program, name, META_ROLE_NONE, NULL);
      if (target && target->generated_by) {
        ASTNode *direct = ast_create_call_expression(
            name, call->arguments, call->argument_count, node->location);
        if (direct) {
          node->type = direct->type;
          node->data = direct->data;
          node->children = direct->children;
          node->child_count = direct->child_count;
        }
      }
    }
  }
  for (size_t i = 0; i < node->child_count; i++) {
    direct_generated_calls(program, node->children[i]);
  }
}

static int generator_round(MetaStage *stage, int *progressed) {
  GenList list = {0};
  *progressed = 0;
  if (!collect_generators(stage, &list)) {
    return 0;
  }
  if (list.count == 0) {
    return 1;
  }
  *progressed = 1;
  int ok = run_generators(stage, &list);
  free(list.items);
  if (ok && !stage->report->pending_error) {
    Program *program = (Program *)stage->program->data;
    for (size_t i = 0; i < program->declaration_count; i++) {
      ASTNode *decl = program->declarations[i];
      if (decl && decl->type == AST_FUNCTION_DECLARATION && decl->data) {
        direct_generated_calls(program,
                               ((FunctionDeclaration *)decl->data)->body);
      }
    }
  }
  return ok;
}

static int stage_round(MetaStage *stage) {
  Program *program = (Program *)stage->program->data;
  LayoutTarget *targets = NULL;
  size_t target_count = 0;
  NameSet names = {0};
  NameSet types = {0};
  int ok = 1;
  for (size_t i = 0; i < program->declaration_count; i++) {
    ASTNode *decl = program->declarations[i];
    if (!decl || decl->type != AST_STRUCT_DECLARATION || !decl->data) {
      continue;
    }
    StructDeclaration *sd = (StructDeclaration *)decl->data;
    if (!struct_pending(sd) || !struct_has_layout_decorator(program, sd)) {
      continue;
    }
    LayoutTarget *grown =
        realloc(targets, (target_count + 1) * sizeof(LayoutTarget));
    if (!grown) {
      return 0;
    }
    targets = grown;
    targets[target_count].decl = decl;
    targets[target_count].sd = sd;
    targets[target_count].done = 0;
    target_count++;
    for (size_t d = 0; d < sd->meta_decorator_count; d++) {
      const FunctionDeclaration *fn = find_function(
          program, sd->meta_decorators[d].name, META_ROLE_LAYOUT, NULL);
      if (fn) {
        name_set_add(&names, fn->name, -1);
      }
    }
  }
  if (target_count == 0) {
    free(targets);
    return 1;
  }
  const FunctionDeclaration *helper = library_function(program, "layout_type");
  if (helper) {
    name_set_add(&names, helper->name, -1);
  }
  helper = library_function(program, "layout_default");
  if (helper) {
    name_set_add(&names, helper->name, -1);
  }
  close_over_functions(program, &names, &types);
  NameSet ambient = {0};
  for (size_t i = 0; i < program->declaration_count; i++) {
    const ASTNode *decl = program->declarations[i];
    if (!decl || decl->type == AST_FUNCTION_DECLARATION) {
      continue;
    }
    collect_names(decl, &names, &ambient, -1);
    if (decl->type == AST_STRUCT_DECLARATION && decl->data) {
      const StructDeclaration *sd = (const StructDeclaration *)decl->data;
      for (size_t m = 0; m < sd->method_count; m++) {
        collect_names(sd->methods[m], &names, &ambient, -1);
      }
    }
  }
  close_over_functions(program, &names, &ambient);
  ok = run_layouts(stage, targets, target_count, &names, &types);
  name_set_free(&names);
  name_set_free(&types);
  name_set_free(&ambient);
  free(targets);
  return ok;
}

int meta_stage_run(ASTNode *program, ErrorReporter *reporter,
                   const char *root_file, const char *source,
                   const char *target, long long fuel,
                   MetaStageReport *report) {
  MetaStage stage;
  Program *data = program && program->type == AST_PROGRAM
                      ? (Program *)program->data
                      : NULL;
  if (!data || (!program_has_decorators(data) &&
                !program_declares_generator_calls(data))) {
    return 1;
  }
  stage.program = program;
  stage.reporter = reporter;
  stage.root_file = root_file;
  stage.source = source;
  stage.target = target;
  stage.fuel = fuel > 0 ? fuel : META_DEFAULT_FUEL;
  stage.report = report;
  if (!check_decorators_known(&stage, data)) {
    return 0;
  }
  for (int round = 0; round < META_MAX_ROUNDS; round++) {
    int progressed = 0;
    if (!stage_round(&stage) || report->pending_error) {
      return report->pending_error ? 1 : 0;
    }
    if (!generator_round(&stage, &progressed) || report->pending_error) {
      return report->pending_error ? 1 : 0;
    }
    if (!progressed) {
      return 1;
    }
  }
  error_reporter_add_error(reporter, ERROR_SEMANTIC, program->location,
                           "Generators kept writing new decorators after 16 "
                           "rounds");
  error_reporter_set_last_code(reporter, "L0001");
  return 0;
}

void meta_stage_print_report(const MetaStageReport *report, FILE *out) {
  if (!report->layouts && !report->generators) {
    fprintf(out, "metaprograms: none; nothing ran\n");
    return;
  }
  fprintf(out,
          "metaprograms: %zu layouts, %zu generators, %zu bytes of source "
          "generated, %zu stage builds, %lld interpreter steps\n",
          report->layouts, report->generators, report->generated_bytes,
          report->stage_builds, report->steps);
}
