#include "type_checker_internal.h"
#include "codegen/target.h"
#include <stdio.h>
#include <time.h>
#include <string.h>

typedef struct {
  TypeChecker *checker;
  const char *why;
  const char *visiting[16];
  size_t visiting_count;
  int depth;
  int per_warp;
} UniformContext;

static int uniform_expression(UniformContext *context, ASTNode *expression);

static ASTNode *uniform_function_body(TypeChecker *checker, const char *name);

static int uniform_body_reads_a_varying_thing(UniformContext *context,
                                              ASTNode *node, int depth) {
  if (!node || depth > 64) {
    return depth > 64;
  }
  if (node->type == AST_FUNCTION_CALL) {
    CallExpression *call = (CallExpression *)node->data;
    if (call && call->function_name) {
      if (strncmp(call->function_name, "gpu_tid_", 8) == 0 ||
          strcmp(call->function_name, "subgroup_local_id") == 0) {
        return 1;
      }
      if (!call->is_gpu_index) {
        ASTNode *callee =
            uniform_function_body(context->checker, call->function_name);
        int already = 0;
        for (size_t i = 0; i < context->visiting_count; i++) {
          if (context->visiting[i] == call->function_name) {
            already = 1;
          }
        }
        if (!callee) {
          return 1;
        }
        if (!already && context->visiting_count < 16) {
          int varying;
          context->visiting[context->visiting_count++] = call->function_name;
          varying = uniform_body_reads_a_varying_thing(context, callee,
                                                       depth + 1);
          context->visiting_count--;
          if (varying) {
            return 1;
          }
        }
      }
    }
  }
  if (node->type == AST_INDEX_EXPRESSION) {
    return 1;
  }
  for (size_t i = 0; i < node->child_count; i++) {
    if (uniform_body_reads_a_varying_thing(context, node->children[i],
                                           depth + 1)) {
      return 1;
    }
  }
  return 0;
}

static ASTNode *uniform_function_body(TypeChecker *checker, const char *name) {
  ASTNode *module = checker->module_program;
  Program *program = module && module->data ? (Program *)module->data : NULL;
  if (!program || !name) {
    return NULL;
  }
  for (size_t i = 0; i < program->declaration_count; i++) {
    ASTNode *declaration = program->declarations[i];
    FunctionDeclaration *function =
        declaration && declaration->type == AST_FUNCTION_DECLARATION
            ? (FunctionDeclaration *)declaration->data
            : NULL;
    if (function && function->name && strcmp(function->name, name) == 0) {
      return function->body;
    }
  }
  return NULL;
}

static int uniform_local_writes(UniformContext *context, ASTNode *node,
                                const char *name, int depth) {
  if (!node || depth > 128) {
    return depth <= 128;
  }
  if (node->type == AST_VAR_DECLARATION) {
    VarDeclaration *declaration = (VarDeclaration *)node->data;
    if (declaration && declaration->name && name &&
        strcmp(declaration->name, name) == 0 && declaration->initializer &&
        !uniform_expression(context, declaration->initializer)) {
      return 0;
    }
  }
  if (node->type == AST_ASSIGNMENT) {
    Assignment *assignment = (Assignment *)node->data;
    if (assignment && assignment->variable_name && name &&
        strcmp(assignment->variable_name, name) == 0 && assignment->value &&
        !uniform_expression(context, assignment->value)) {
      return 0;
    }
  }
  for (size_t i = 0; i < node->child_count; i++) {
    if (!uniform_local_writes(context, node->children[i], name, depth + 1)) {
      return 0;
    }
  }
  return 1;
}

static int uniform_identifier(UniformContext *context, ASTNode *expression) {
  Identifier *identifier = (Identifier *)expression->data;
  TypeChecker *checker = context->checker;
  Symbol *symbol = identifier && identifier->name
                       ? symbol_table_lookup(checker->symbol_table,
                                             identifier->name)
                       : NULL;
  ASTNode *owner_node = checker->current_function_decl;
  FunctionDeclaration *owner =
      owner_node && owner_node->type == AST_FUNCTION_DECLARATION
          ? (FunctionDeclaration *)owner_node->data
          : NULL;
  if (!symbol) {
    context->why = identifier && identifier->name ? identifier->name
                                                  : "an unknown name";
    return 0;
  }
  if (symbol->type && symbol->type->refine_uniform) {
    return 1;
  }
  if (symbol->kind == SYMBOL_PARAMETER) {
    if (owner && owner->is_kernel) {
      return 1;
    }
    context->why = identifier->name;
    return 0;
  }
  if (symbol->kind == SYMBOL_CONSTANT) {
    return 1;
  }
  if (symbol->kind == SYMBOL_VARIABLE && symbol->scope &&
      symbol->scope->type == SCOPE_GLOBAL) {
    context->why = identifier->name;
    return 0;
  }
  if (symbol->kind == SYMBOL_VARIABLE || symbol->kind == SYMBOL_CONSTANT) {
    for (size_t i = 0; i < context->visiting_count; i++) {
      if (context->visiting[i] == identifier->name) {
        return 1;
      }
    }
    if (!owner || !owner->body || context->visiting_count >= 16) {
      context->why = identifier->name;
      return 0;
    }
    context->visiting[context->visiting_count++] = identifier->name;
    {
      int uniform =
          uniform_local_writes(context, owner->body, identifier->name, 0);
      context->visiting_count--;
      if (!uniform && !context->why) {
        context->why = identifier->name;
      }
      return uniform;
    }
  }
  context->why = identifier->name;
  return 0;
}

static int uniform_call(UniformContext *context, ASTNode *expression) {
  CallExpression *call = (CallExpression *)expression->data;
  ASTNode *body;
  if (!call || !call->function_name) {
    context->why = "an indirect call";
    return 0;
  }
  if (call->is_gpu_index) {
    if (strncmp(call->function_name, "gpu_tid_", 8) == 0) {
      static const char *const axis[3] = {"thread.x", "thread.y", "thread.z"};
      char last = call->function_name[8];
      context->why = last == 'y' ? axis[1] : last == 'z' ? axis[2] : axis[0];
      return 0;
    }
    return 1;
  }
  if (strcmp(call->function_name, "subgroup_local_id") == 0) {
    context->why = "subgroup_local_id()";
    return 0;
  }
  if (strcmp(call->function_name, "subgroup_size") == 0) {
    return 1;
  }
  for (size_t i = 0; i < call->argument_count; i++) {
    if (!uniform_expression(context, call->arguments[i])) {
      return 0;
    }
  }
  body = uniform_function_body(context->checker, call->function_name);
  if (!body) {
    context->why = call->function_name;
    return 0;
  }
  if (uniform_body_reads_a_varying_thing(context, body, 0)) {
    context->why = call->function_name;
    return 0;
  }
  return 1;
}

static int uniform_warp_index(UniformContext *context, BinaryExpression *binary) {
  ASTNode *owner_node = context->checker->current_function_decl;
  FunctionDeclaration *owner =
      owner_node && owner_node->type == AST_FUNCTION_DECLARATION
          ? (FunctionDeclaration *)owner_node->data
          : NULL;
  CallExpression *index;
  NumberLiteral *amount;
  long long rows;
  if (!context->per_warp || !binary || !binary->operator || !owner ||
      !owner->is_kernel || !binary->left || !binary->right ||
      binary->left->type != AST_FUNCTION_CALL ||
      binary->right->type != AST_NUMBER_LITERAL) {
    return 0;
  }
  index = (CallExpression *)binary->left->data;
  amount = (NumberLiteral *)binary->right->data;
  if (!index || !index->function_name || !amount || amount->is_float ||
      strcmp(index->function_name, "gpu_tid_x") != 0) {
    return 0;
  }
  rows = (owner->kernel_block[1] > 1 ? owner->kernel_block[1] : 1) *
         (owner->kernel_block[2] > 1 ? owner->kernel_block[2] : 1);
  if (owner->kernel_block[0] <= 0 ||
      (rows != 1 && owner->kernel_block[0] % 32 != 0)) {
    return 0;
  }
  if (strcmp(binary->operator, ">>") == 0) {
    return amount->int_value == 5;
  }
  return strcmp(binary->operator, "/") == 0 && amount->int_value == 32;
}

static int uniform_expression(UniformContext *context, ASTNode *expression) {
  if (!expression || context->depth > 64) {
    return expression == NULL;
  }
  context->depth++;
  switch (expression->type) {
  case AST_NUMBER_LITERAL:
  case AST_STRING_LITERAL:
    context->depth--;
    return 1;
  case AST_IDENTIFIER: {
    int uniform = uniform_identifier(context, expression);
    context->depth--;
    return uniform;
  }
  case AST_FUNCTION_CALL: {
    int uniform = uniform_call(context, expression);
    context->depth--;
    return uniform;
  }
  case AST_BINARY_EXPRESSION: {
    BinaryExpression *binary = (BinaryExpression *)expression->data;
    int uniform = binary && (uniform_warp_index(context, binary) ||
                             (uniform_expression(context, binary->left) &&
                              uniform_expression(context, binary->right)));
    context->depth--;
    return uniform;
  }
  case AST_UNARY_EXPRESSION: {
    UnaryExpression *unary = (UnaryExpression *)expression->data;
    int uniform = unary && uniform_expression(context, unary->operand);
    context->depth--;
    return uniform;
  }
  case AST_CAST_EXPRESSION: {
    CastExpression *cast = (CastExpression *)expression->data;
    int uniform = cast && uniform_expression(context, cast->operand);
    context->depth--;
    return uniform;
  }
  case AST_MEMBER_ACCESS: {
    MemberAccess *member = (MemberAccess *)expression->data;
    int uniform = member && uniform_expression(context, member->object);
    context->depth--;
    return uniform;
  }
  case AST_INDEX_EXPRESSION:
    context->why = "a load from memory";
    context->depth--;
    return 0;
  default:
    context->why = "an expression whose value the compiler cannot follow";
    context->depth--;
    return 0;
  }
}

int type_checker_expression_surely_varies(TypeChecker *checker,
                                          ASTNode *expression, int depth) {
  if (!checker || !expression || depth > 64) {
    return 0;
  }
  switch (expression->type) {
  case AST_IDENTIFIER: {
    Identifier *identifier = (Identifier *)expression->data;
    Symbol *symbol = identifier && identifier->name
                         ? symbol_table_lookup(checker->symbol_table,
                                               identifier->name)
                         : NULL;
    ASTNode *owner_node = checker->current_function_decl;
    FunctionDeclaration *owner =
        owner_node && owner_node->type == AST_FUNCTION_DECLARATION
            ? (FunctionDeclaration *)owner_node->data
            : NULL;
    if (!symbol) {
      return 1;
    }
    if (symbol->type && symbol->type->refine_uniform) {
      return 0;
    }
    if (symbol->kind == SYMBOL_PARAMETER) {
      return !(owner && owner->is_kernel);
    }
    if (symbol->kind == SYMBOL_CONSTANT) {
      return 0;
    }
    if (symbol->kind == SYMBOL_VARIABLE) {
      return symbol->scope && symbol->scope->type == SCOPE_GLOBAL;
    }
    return 1;
  }
  case AST_BINARY_EXPRESSION: {
    BinaryExpression *binary = (BinaryExpression *)expression->data;
    return !binary ||
           type_checker_expression_surely_varies(checker, binary->left,
                                                 depth + 1) ||
           type_checker_expression_surely_varies(checker, binary->right,
                                                 depth + 1);
  }
  case AST_UNARY_EXPRESSION: {
    UnaryExpression *unary = (UnaryExpression *)expression->data;
    return !unary || type_checker_expression_surely_varies(
                         checker, unary->operand, depth + 1);
  }
  case AST_CAST_EXPRESSION: {
    CastExpression *cast = (CastExpression *)expression->data;
    return !cast || type_checker_expression_surely_varies(
                        checker, cast->operand, depth + 1);
  }
  case AST_MEMBER_ACCESS: {
    MemberAccess *member = (MemberAccess *)expression->data;
    return !member || type_checker_expression_surely_varies(
                          checker, member->object, depth + 1);
  }
  case AST_NUMBER_LITERAL:
  case AST_STRING_LITERAL:
  case AST_FUNCTION_CALL:
    return 0;
  default:
    return 1;
  }
}

int type_checker_expression_is_uniform(TypeChecker *checker,
                                       ASTNode *expression,
                                       const char **why) {
  UniformContext context;
  int uniform;
  if (why) {
    *why = NULL;
  }
  if (!checker || !expression) {
    return 0;
  }
  memset(&context, 0, sizeof(context));
  context.checker = checker;
  uniform = uniform_expression(&context, expression);
  if (!uniform && why) {
    *why = context.why ? context.why : "a value that varies by work item";
  }
  return uniform;
}

int type_checker_expression_is_warp_uniform(TypeChecker *checker,
                                            ASTNode *expression) {
  UniformContext context;
  if (!checker || !expression) {
    return 0;
  }
  memset(&context, 0, sizeof(context));
  context.checker = checker;
  context.per_warp = 1;
  return uniform_expression(&context, expression);
}

typedef struct {
  const char *owner;
  const char *binding;
  const char *type_name;
  size_t line;
  int proof;
  char detail[160];
} GpuTypeNote;

static GpuTypeNote g_gpu_notes[256];
static size_t g_gpu_note_count = 0;
static size_t g_gpu_note_total = 0;
static int g_gpu_report = 0;
static double g_gpu_report_seconds = 0.0;

void type_checker_set_gpu_type_report(int enabled) {
  g_gpu_report = enabled;
  g_gpu_note_count = 0;
  g_gpu_note_total = 0;
  g_gpu_report_seconds = 0.0;
}

static const char *gpu_space_word(unsigned char space) {
  switch (space) {
  case DEVICE_SPACE_GLOBAL: return "global";
  case DEVICE_SPACE_SHARED: return "shared";
  case DEVICE_SPACE_CONSTANT: return "constant";
  case DEVICE_SPACE_LOCAL: return "local";
  case DEVICE_SPACE_GENERIC: return "generic";
  default: return "unstated";
  }
}

static const char *gpu_layout_word(unsigned char layout) {
  switch (layout) {
  case VIEW_LAYOUT_ROW: return "row";
  case VIEW_LAYOUT_COL: return "col";
  case VIEW_LAYOUT_SWIZZLE32: return "swizzle32";
  case VIEW_LAYOUT_SWIZZLE64: return "swizzle64";
  case VIEW_LAYOUT_SWIZZLE128: return "swizzle128";
  case VIEW_LAYOUT_INTERLEAVE: return "interleave";
  case VIEW_LAYOUT_FRAGMENT_A: return "fragment_a";
  case VIEW_LAYOUT_FRAGMENT_B: return "fragment_b";
  case VIEW_LAYOUT_FRAGMENT_C: return "fragment_c";
  default: return "row";
  }
}

void type_checker_note_device_type_in(TypeChecker *checker,
                                      const char *binding, const Type *type,
                                      unsigned char space_hint,
                                      SourceLocation where) {
  GpuTypeNote *note;
  ASTNode *owner_node;
  FunctionDeclaration *owner;
  if (!g_gpu_report || !checker || !type) {
    return;
  }
  if (!type->device_space && !type->declared_align && !type->view_layout &&
      !type->view_extents[0]) {
    return;
  }
  g_gpu_note_total++;
  if (g_gpu_note_count >= 256) {
    return;
  }
  owner_node = checker->current_function_decl;
  owner = owner_node && owner_node->type == AST_FUNCTION_DECLARATION
              ? (FunctionDeclaration *)owner_node->data
              : NULL;
  note = &g_gpu_notes[g_gpu_note_count++];
  note->owner = owner ? owner->name : NULL;
  note->binding = binding;
  note->type_name = type->name;
  note->line = where.line;
  note->proof = 0;
  {
    unsigned char space =
        type->device_space ? type->device_space : space_hint;
    if (type->view_extents[0]) {
      snprintf(note->detail, sizeof(note->detail),
               "%s memory, laid out %s, extents %zu by %zu",
               gpu_space_word(space), gpu_layout_word(type->view_layout),
               type->view_extents[0], type->view_extents[1]);
    } else if (type->declared_align) {
      snprintf(note->detail, sizeof(note->detail),
               "%s memory, %zu-byte aligned", gpu_space_word(space),
               type->declared_align);
    } else {
      snprintf(note->detail, sizeof(note->detail), "%s memory",
               gpu_space_word(space));
    }
  }
}

void type_checker_note_device_type(TypeChecker *checker, const char *binding,
                                   const Type *type, SourceLocation where) {
  type_checker_note_device_type_in(checker, binding, type, DEVICE_SPACE_NONE,
                                   where);
}

static void gpu_note_proof(TypeChecker *checker, const Type *view,
                           SourceLocation where, const char *detail) {
  GpuTypeNote *note;
  ASTNode *owner_node;
  FunctionDeclaration *owner;
  if (!g_gpu_report || !checker) {
    return;
  }
  g_gpu_note_total++;
  if (g_gpu_note_count >= 256) {
    return;
  }
  owner_node = checker->current_function_decl;
  owner = owner_node && owner_node->type == AST_FUNCTION_DECLARATION
              ? (FunctionDeclaration *)owner_node->data
              : NULL;
  note = &g_gpu_notes[g_gpu_note_count++];
  note->owner = owner ? owner->name : NULL;
  note->binding = NULL;
  note->type_name = view ? view->name : NULL;
  note->line = where.line;
  note->proof = 1;
  snprintf(note->detail, sizeof(note->detail), "%s", detail);
}

size_t type_checker_gpu_type_note_total(void) { return g_gpu_note_total; }

int type_checker_gpu_type_note_at(size_t index, size_t *line,
                                  const char **owner, const char **detail) {
  if (index >= g_gpu_note_count) {
    return 0;
  }
  *line = g_gpu_notes[index].line;
  *owner = g_gpu_notes[index].owner;
  *detail = g_gpu_notes[index].detail;
  return 1;
}

void type_checker_print_gpu_type_report(FILE *out) {
  if (!g_gpu_report || !out) {
    return;
  }
  fprintf(out, "proven by type\n");
  if (g_gpu_note_count == 0) {
    fprintf(out, "  nothing in this module states where its memory lives, how "
                 "it is laid out, or which values a group shares\n");
    return;
  }
  for (size_t i = 0; i < g_gpu_note_count; i++) {
    const GpuTypeNote *note = &g_gpu_notes[i];
    fprintf(out, "  line %zu%s%s: %s%s%s%s\n", note->line,
            note->owner ? " in " : "", note->owner ? note->owner : "",
            note->binding ? note->binding : "",
            note->binding ? " is " : "",
            note->proof ? "" : "",
            note->detail);
  }
  if (g_gpu_note_total > g_gpu_note_count) {
    fprintf(out, "  (%zu more not listed)\n",
            g_gpu_note_total - g_gpu_note_count);
  }
  fprintf(out, "  the device type analyses took %.3f ms\n",
          g_gpu_report_seconds * 1000.0);
}

static int lane_value(TypeChecker *checker, ASTNode *expression, long long lane,
                      int depth, long long *out);

static int lane_local_value(TypeChecker *checker, ASTNode *node,
                            const char *name, long long lane, int depth,
                            int *found, long long *out) {
  if (!node || depth > 128) {
    return 1;
  }
  if (node->type == AST_VAR_DECLARATION) {
    VarDeclaration *declaration = (VarDeclaration *)node->data;
    if (declaration && declaration->name && name &&
        strcmp(declaration->name, name) == 0) {
      if (*found || !declaration->initializer ||
          !lane_value(checker, declaration->initializer, lane, depth + 1,
                      out)) {
        return 0;
      }
      *found = 1;
    }
  }
  if (node->type == AST_ASSIGNMENT) {
    Assignment *assignment = (Assignment *)node->data;
    if (assignment && assignment->variable_name && name &&
        strcmp(assignment->variable_name, name) == 0) {
      return 0;
    }
  }
  for (size_t i = 0; i < node->child_count; i++) {
    if (!lane_local_value(checker, node->children[i], name, lane, depth + 1,
                          found, out)) {
      return 0;
    }
  }
  return 1;
}

static int lane_binary_value(TypeChecker *checker, ASTNode *expression, long long lane,
                      int depth, long long *out) {
    BinaryExpression *binary = (BinaryExpression *)expression->data;
    long long left = 0;
    long long right = 0;
    if (!binary || !binary->operator ||
        !lane_value(checker, binary->left, lane, depth + 1, &left) ||
        !lane_value(checker, binary->right, lane, depth + 1, &right)) {
      return 0;
    }
    if (strcmp(binary->operator, "+") == 0) { *out = left + right; return 1; }
    if (strcmp(binary->operator, "-") == 0) { *out = left - right; return 1; }
    if (strcmp(binary->operator, "*") == 0) { *out = left * right; return 1; }
    if (strcmp(binary->operator, "^") == 0) { *out = left ^ right; return 1; }
    if (strcmp(binary->operator, "&") == 0) { *out = left & right; return 1; }
    if (strcmp(binary->operator, "|") == 0) { *out = left | right; return 1; }
    if (strcmp(binary->operator, "<<") == 0 && right >= 0 && right < 32) {
      *out = left << right;
      return 1;
    }
    if (strcmp(binary->operator, ">>") == 0 && right >= 0 && right < 32) {
      *out = left >> right;
      return 1;
    }
    if (strcmp(binary->operator, "/") == 0 && right != 0) {
      *out = left / right;
      return 1;
    }
    if (strcmp(binary->operator, "%") == 0 && right != 0) {
      *out = left % right;
      return 1;
    }
    return 0;
}

static int lane_value(TypeChecker *checker, ASTNode *expression, long long lane,
                      int depth, long long *out) {
  long long constant = 0;
  if (!expression || depth > 32) {
    return 0;
  }
  if (type_checker_eval_integer_constant_with_checker(checker, expression,
                                                      &constant)) {
    *out = constant;
    return 1;
  }
  switch (expression->type) {
  case AST_FUNCTION_CALL: {
    CallExpression *call = (CallExpression *)expression->data;
    if (call && call->is_gpu_index && call->function_name &&
        strcmp(call->function_name, "gpu_tid_x") == 0) {
      *out = lane;
      return 1;
    }
    if (call && call->function_name &&
        strcmp(call->function_name, "subgroup_local_id") == 0) {
      *out = lane;
      return 1;
    }
    return 0;
  }
  case AST_CAST_EXPRESSION: {
    CastExpression *cast = (CastExpression *)expression->data;
    return cast && lane_value(checker, cast->operand, lane, depth + 1, out);
  }
  case AST_BINARY_EXPRESSION:
    return lane_binary_value(checker, expression, lane, depth, out);

  case AST_IDENTIFIER: {
    Identifier *identifier = (Identifier *)expression->data;
    ASTNode *owner_node = checker->current_function_decl;
    FunctionDeclaration *owner =
        owner_node && owner_node->type == AST_FUNCTION_DECLARATION
            ? (FunctionDeclaration *)owner_node->data
            : NULL;
    int found = 0;
    if (!identifier || !identifier->name || !owner || !owner->body) {
      return 0;
    }
    if (!lane_local_value(checker, owner->body, identifier->name, lane, 0,
                          &found, out)) {
      return 0;
    }
    return found;
  }
  default:
    break;
  }
  return 0;
}

long long type_checker_view_element_offset(const Type *view, long long row,
                                           long long column) {
  long long rows = (long long)view->view_extents[0];
  long long columns = (long long)view->view_extents[1];
  long long element_size = view->base_type ? (long long)view->base_type->size : 4;
  switch (view->view_layout) {
  case VIEW_LAYOUT_COL:
    return column * rows + row;
  case VIEW_LAYOUT_INTERLEAVE: {
    long long group = view->view_layout_param;
    if (group <= 0) {
      return row * columns + column;
    }
    return (column / group) * (rows * group) + row * group + (column % group);
  }
  case VIEW_LAYOUT_SWIZZLE32:
  case VIEW_LAYOUT_SWIZZLE64:
  case VIEW_LAYOUT_SWIZZLE128: {
    long long chunk_bytes = view->view_layout == VIEW_LAYOUT_SWIZZLE32   ? 4
                            : view->view_layout == VIEW_LAYOUT_SWIZZLE64 ? 8
                                                                         : 16;
    long long chunk = element_size ? chunk_bytes / element_size : 1;
    long long chunks_per_row = chunk > 0 ? columns / chunk : 0;
    if (chunk <= 0 || chunks_per_row <= 0) {
      return row * columns + column;
    }
    return row * columns + ((column / chunk) ^ (row % chunks_per_row)) * chunk +
           (column % chunk);
  }
  default:
    break;
  }
  return row * columns + column;
}

typedef struct {
  TypeChecker *checker;
  int failed;
  SourceLocation where;
  char detail[224];
} ConflictContext;

static void conflict_check_access(ConflictContext *context, ASTNode *outer,
                                  ASTNode *inner_node, Type *view) {
  ArrayIndexExpression *outer_index = (ArrayIndexExpression *)outer->data;
  ArrayIndexExpression *inner_index = (ArrayIndexExpression *)inner_node->data;
  const MtlcTargetDescription *machine = mtlc_target_current_description();
  int width = machine && machine->subgroup_width > 0 ? machine->subgroup_width
                                                     : 32;
  int banks = machine && machine->shared_memory_banks > 0
                  ? machine->shared_memory_banks
                  : 32;
  int bank_bytes = machine && machine->shared_bank_bytes > 0
                       ? machine->shared_bank_bytes
                       : 4;
  long long element_size = view->base_type ? (long long)view->base_type->size : 4;
  long long seen[128];
  long long fixed_row = 0;
  long long fixed_column = 0;
  int row_from_lane = lane_value(context->checker, inner_index->index, 0, 0,
                                 &fixed_row);
  int column_from_lane = lane_value(context->checker, outer_index->index, 0, 0,
                                    &fixed_column);
  long long sweep_extent = 1;
  int sweep_row = 0;
  int sweep_column = 0;
  if (context->failed || width <= 0 || width > 128) {
    return;
  }
  if (!row_from_lane) {
    if (!type_checker_expression_is_uniform(context->checker,
                                            inner_index->index, NULL)) {
      context->failed = 1;
      snprintf(context->detail, sizeof(context->detail),
               "the index does not follow from the work item alone, so the "
               "addresses one subgroup touches are not known here");
      context->where = outer->location;
      return;
    }
    sweep_row = 1;
    sweep_extent = (long long)view->view_extents[0];
  }
  if (!column_from_lane) {
    if (sweep_row ||
        !type_checker_expression_is_uniform(context->checker,
                                            outer_index->index, NULL)) {
      context->failed = 1;
      snprintf(context->detail, sizeof(context->detail),
               "the index does not follow from the work item alone, so the "
               "addresses one subgroup touches are not known here");
      context->where = outer->location;
      return;
    }
    sweep_column = 1;
    sweep_extent = (long long)view->view_extents[1];
  }
  if (sweep_extent <= 0 || sweep_extent > 4096) {
    context->failed = 1;
    snprintf(context->detail, sizeof(context->detail),
             "the view's extent is not a size this proof can walk");
    context->where = outer->location;
    return;
  }
  for (long long fixed = 0; fixed < sweep_extent; fixed++) {
    for (long long lane = 0; lane < width; lane++) {
      long long row = 0;
      long long column = 0;
      long long offset;
      long long bank;
      if (sweep_row) {
        row = fixed;
      } else if (!lane_value(context->checker, inner_index->index, lane, 0,
                             &row)) {
        context->failed = 1;
        snprintf(context->detail, sizeof(context->detail),
                 "the index does not follow from the work item alone, so the "
                 "addresses one subgroup touches are not known here");
        context->where = outer->location;
        return;
      }
      if (sweep_column) {
        column = fixed;
      } else if (!lane_value(context->checker, outer_index->index, lane, 0,
                             &column)) {
        context->failed = 1;
        snprintf(context->detail, sizeof(context->detail),
                 "the index does not follow from the work item alone, so the "
                 "addresses one subgroup touches are not known here");
        context->where = outer->location;
        return;
      }
      offset = type_checker_view_element_offset(view, row, column);
      bank = ((offset * element_size) / bank_bytes) % banks;
      for (long long earlier = 0; earlier < lane; earlier++) {
        if (seen[earlier] == bank) {
          context->failed = 1;
          snprintf(context->detail, sizeof(context->detail),
                   "work items %lld and %lld both land in bank %lld, so this "
                   "access is two accesses",
                   earlier, lane, bank);
          context->where = outer->location;
          return;
        }
      }
      seen[lane] = bank;
    }
  }
  {
    char detail[160];
    snprintf(detail, sizeof(detail),
             "one subgroup's %d addresses through '%s' fall in %d distinct "
             "banks",
             width, view->name ? view->name : "the view", width);
    gpu_note_proof(context->checker, view, outer->location, detail);
  }
}

static void conflict_walk(ConflictContext *context, ASTNode *node, int depth) {
  if (!node || context->failed || depth > 128) {
    return;
  }
  if (node->type == AST_INDEX_EXPRESSION) {
    ArrayIndexExpression *outer = (ArrayIndexExpression *)node->data;
    if (outer && outer->array && outer->array->type == AST_INDEX_EXPRESSION) {
      ArrayIndexExpression *inner = (ArrayIndexExpression *)outer->array->data;
      Type *view = inner && inner->array ? inner->array->resolved_type : NULL;
      if (view && view->kind == TYPE_SLICE && view->view_extents[0] > 0 &&
          type_view_rank(view) == 2 &&
          (view->device_space == DEVICE_SPACE_SHARED ||
           type_checker_lvalue_device_space(context->checker, inner->array) ==
               DEVICE_SPACE_SHARED)) {
        conflict_check_access(context, node, outer->array, view);
      }
    }
  }
  for (size_t i = 0; i < node->child_count; i++) {
    conflict_walk(context, node->children[i], depth + 1);
  }
}

int type_checker_check_conflict_free(TypeChecker *checker, ASTNode *statement) {
  ConflictContext context;
  clock_t started;
  if (!checker || !statement || !statement->conflict_free_mode) {
    return 1;
  }
  memset(&context, 0, sizeof(context));
  context.checker = checker;
  context.where = statement->location;
  started = clock();
  conflict_walk(&context, statement, 0);
  g_gpu_report_seconds +=
      (double)(clock() - started) / (double)CLOCKS_PER_SEC;
  if (!context.failed) {
    return 1;
  }
  if (statement->conflict_free_mode == 2) {
    type_checker_set_error_at_location(
        checker, context.where,
        "'@conflict_free!' says one subgroup's addresses fall in distinct "
        "workgroup banks, and %s",
        context.detail);
    return 0;
  }
  if (checker->error_reporter) {
    char message[288];
    snprintf(message, sizeof(message),
             "'@conflict_free' was asked for and %s", context.detail);
    error_reporter_add_warning(checker->error_reporter, ERROR_SEMANTIC,
                               context.where, message);
  }
  return 1;
}
