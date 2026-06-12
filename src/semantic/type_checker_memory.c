// Compile-time memory diagnostics: the analyses that catch the bugs people
// otherwise find at 2am with a hex dump. Runs per function, right after the
// body type-checks, while the function's scope (parameters, const locals)
// is still live.
//
//   M0101  use of a pointer after free(p)                       warning
//   M0102  double free                                          warning
//   M0103  returning the address of a stack local               ERROR
//   M0104  storing the address of a stack local in a global     warning
//   M0105  constant array index out of bounds                   ERROR
//   M0106  constant-size memory op overflowing a stack array    ERROR
//   M0107  allocation that never escapes and is never freed     warning
//
// The analysis is deliberately conservative: definite-bug states are only
// set on the function's straight-line spine; anything that happens inside a
// branch or loop demotes to "maybe" and stays silent. A diagnostic from this
// file is meant to be trusted, so the false-positive budget is zero.

#include "type_checker_internal.h"

#define MEM_MAX_LOCALS 256

typedef enum {
  MEM_FREED_NO = 0,
  MEM_FREED_DEFINITE = 1, /* freed on the spine: later use IS a bug */
  MEM_FREED_MAYBE = 2     /* freed inside a branch/loop: stay silent */
} MemFreedState;

typedef struct {
  const char *name;      /* AST-owned */
  const char *type_name; /* AST-owned */
  SourceLocation decl_loc;
  int is_param;
  int is_stack;   /* array/struct/scalar local: its address dies with the frame */
  int is_pointer; /* trailing '*', cstring, or string (which carries a pointer) */
  MemFreedState freed;
  SourceLocation freed_loc;
  int holds_alloc; /* assigned from malloc/calloc/realloc/new on the spine */
  SourceLocation alloc_loc;
  int escaped;     /* returned, stored, passed to a call, or address taken */
  int ever_freed;  /* a free()/realloc() of this pointer appears ANYWHERE */
  const char *points_to_stack; /* stack local whose address it holds, or NULL */
} MemLocal;

typedef struct {
  TypeChecker *checker;
  FunctionDeclaration *fn;
  SourceLocation fn_loc;
  MemLocal locals[MEM_MAX_LOCALS];
  size_t local_count;
  int depth;      /* 0 = the function's straight-line spine */
  int in_defer;   /* defers run at scope exit: record facts, never report */
  int fn_returns_pointer;
  int had_error;
} MemCtx;

/* ---- small type helpers ----------------------------------------------------- */

static int mem_type_is_pointer(const char *type_name) {
  size_t len = type_name ? strlen(type_name) : 0;
  if (len == 0) {
    return 0;
  }
  if (type_name[len - 1] == '*') {
    return 1;
  }
  return strcmp(type_name, "cstring") == 0;
}

static long long mem_scalar_size(MemCtx *ctx, const char *type_name) {
  if (!type_name) {
    return 0;
  }
  if (mem_type_is_pointer(type_name)) {
    return 8;
  }
  if (strcmp(type_name, "int8") == 0 || strcmp(type_name, "uint8") == 0 ||
      strcmp(type_name, "bool") == 0) {
    return 1;
  }
  if (strcmp(type_name, "int16") == 0 || strcmp(type_name, "uint16") == 0) {
    return 2;
  }
  if (strcmp(type_name, "int32") == 0 || strcmp(type_name, "uint32") == 0 ||
      strcmp(type_name, "float32") == 0) {
    return 4;
  }
  if (strcmp(type_name, "int64") == 0 || strcmp(type_name, "uint64") == 0 ||
      strcmp(type_name, "float64") == 0) {
    return 8;
  }
  if (strcmp(type_name, "string") == 0) {
    return 16;
  }
  Type *type = type_checker_get_type_by_name(ctx->checker, (char *)type_name);
  return type && type->size > 0 ? (long long)type->size : 0;
}

/* Parse `Elem[N]` out of a declared type. Returns 1 with the element count
 * and byte size on success. Multi-dimensional arrays are left alone. */
static int mem_array_extent(MemCtx *ctx, const char *type_name,
                            long long *count_out, long long *elem_size_out) {
  const char *bracket = type_name ? strchr(type_name, '[') : NULL;
  if (!bracket || bracket == type_name || strchr(bracket + 1, '[')) {
    return 0;
  }
  char *end = NULL;
  long long n = strtoll(bracket + 1, &end, 10);
  if (!end || *end != ']' || n <= 0) {
    return 0;
  }
  char elem[96];
  size_t elem_len = (size_t)(bracket - type_name);
  if (elem_len >= sizeof(elem)) {
    return 0;
  }
  memcpy(elem, type_name, elem_len);
  elem[elem_len] = '\0';
  long long elem_size = mem_scalar_size(ctx, elem);
  if (elem_size <= 0) {
    return 0;
  }
  *count_out = n;
  *elem_size_out = elem_size;
  return 1;
}

/* ---- local table ------------------------------------------------------------- */

static MemLocal *mem_find_local(MemCtx *ctx, const char *name) {
  if (!name) {
    return NULL;
  }
  for (size_t i = ctx->local_count; i > 0; i--) {
    if (strcmp(ctx->locals[i - 1].name, name) == 0) {
      return &ctx->locals[i - 1];
    }
  }
  return NULL;
}

static MemLocal *mem_add_local(MemCtx *ctx, const char *name,
                               const char *type_name, SourceLocation loc,
                               int is_param) {
  if (!name || !type_name || ctx->local_count >= MEM_MAX_LOCALS) {
    return NULL;
  }
  MemLocal *local = &ctx->locals[ctx->local_count++];
  memset(local, 0, sizeof(*local));
  local->name = name;
  local->type_name = type_name;
  local->decl_loc = loc;
  local->is_param = is_param;
  local->is_pointer = mem_type_is_pointer(type_name);
  local->is_stack = !is_param && !local->is_pointer &&
                    strncmp(type_name, "fn", 2) != 0;
  return local;
}

/* ---- diagnostics -------------------------------------------------------------- */

static void mem_warn(MemCtx *ctx, SourceLocation loc, const char *fmt, ...) {
  char message[512];
  va_list args;
  va_start(args, fmt);
  vsnprintf(message, sizeof(message), fmt, args);
  va_end(args);
  error_reporter_add_warning(ctx->checker->error_reporter, ERROR_SEMANTIC, loc,
                             message);
}

static void mem_error(MemCtx *ctx, SourceLocation loc, const char *suggestion,
                      const char *fmt, ...) {
  char message[512];
  va_list args;
  va_start(args, fmt);
  vsnprintf(message, sizeof(message), fmt, args);
  va_end(args);
  error_reporter_add_error_with_suggestion(ctx->checker->error_reporter,
                                           ERROR_SEMANTIC, loc, message,
                                           suggestion);
  ctx->checker->has_error = 1;
  ctx->had_error = 1;
}

/* ---- expression classification ----------------------------------------------- */

static ASTNode *mem_unwrap_cast(ASTNode *node) {
  int guard = 0;
  while (node && node->type == AST_CAST_EXPRESSION && guard++ < 8) {
    CastExpression *cast = (CastExpression *)node->data;
    node = cast ? cast->operand : NULL;
  }
  return node;
}

/* The stack local at the root of `&expr` (e.g. `&buf`, `&buf[i]`,
 * `&point.x`), or NULL when the expression is not an address of frame
 * memory. A dereference anywhere in the chain breaks it: `&p[i]` where p is
 * a pointer addresses the pointee. */
static MemLocal *mem_addr_of_stack(MemCtx *ctx, ASTNode *expr) {
  expr = mem_unwrap_cast(expr);
  if (!expr || expr->type != AST_UNARY_EXPRESSION) {
    return NULL;
  }
  UnaryExpression *unary = (UnaryExpression *)expr->data;
  if (!unary || !unary->operator || strcmp(unary->operator, "&") != 0) {
    return NULL;
  }
  ASTNode *node = unary->operand;
  int guard = 0;
  while (node && guard++ < 16) {
    node = mem_unwrap_cast(node);
    if (!node) {
      return NULL;
    }
    if (node->type == AST_IDENTIFIER) {
      Identifier *id = (Identifier *)node->data;
      MemLocal *local = id ? mem_find_local(ctx, id->name) : NULL;
      return (local && local->is_stack) ? local : NULL;
    }
    if (node->type == AST_INDEX_EXPRESSION) {
      ArrayIndexExpression *index = (ArrayIndexExpression *)node->data;
      node = index ? index->array : NULL;
      continue;
    }
    if (node->type == AST_MEMBER_ACCESS) {
      MemberAccess *member = (MemberAccess *)node->data;
      node = member ? member->object : NULL;
      continue;
    }
    return NULL;
  }
  return NULL;
}

/* True when `expr` produces a fresh heap allocation. */
static int mem_is_allocation(ASTNode *expr) {
  expr = mem_unwrap_cast(expr);
  if (!expr) {
    return 0;
  }
  if (expr->type == AST_NEW_EXPRESSION) {
    return 1;
  }
  if (expr->type == AST_FUNCTION_CALL) {
    CallExpression *call = (CallExpression *)expr->data;
    return call && call->function_name && !call->object &&
           (strcmp(call->function_name, "malloc") == 0 ||
            strcmp(call->function_name, "calloc") == 0 ||
            strcmp(call->function_name, "realloc") == 0);
  }
  return 0;
}

static MemLocal *mem_expr_as_local(MemCtx *ctx, ASTNode *expr) {
  expr = mem_unwrap_cast(expr);
  if (!expr || expr->type != AST_IDENTIFIER) {
    return NULL;
  }
  Identifier *id = (Identifier *)expr->data;
  return id ? mem_find_local(ctx, id->name) : NULL;
}

/* ---- the expression walk -------------------------------------------------------
 * One pass per expression: flags use-after-free on every read of a freed
 * pointer, bounds-checks constant indexes into stack arrays, classifies
 * free()/realloc() calls, marks escapes, and checks constant-size memory
 * ops against their destination's capacity. */

static void mem_walk_expr(MemCtx *ctx, ASTNode *expr);

static void mem_check_use(MemCtx *ctx, MemLocal *local, SourceLocation loc) {
  if (!local || ctx->in_defer) {
    return;
  }
  if (local->freed == MEM_FREED_DEFINITE) {
    mem_warn(ctx, loc,
             "Use of `%s` after it was freed (freed at line %zu); this is "
             "use-after-free",
             local->name, local->freed_loc.line);
    local->freed = MEM_FREED_MAYBE; /* one report per free site */
  }
}

static void mem_handle_free(MemCtx *ctx, ASTNode *arg, SourceLocation loc) {
  MemLocal *local = mem_expr_as_local(ctx, arg);
  if (!local || !local->is_pointer) {
    return;
  }
  local->ever_freed = 1;
  if (ctx->in_defer) {
    return; /* defers run at scope exit; their free is not a flow event */
  }
  if (local->freed == MEM_FREED_DEFINITE) {
    mem_warn(ctx, loc,
             "Double free of `%s` (already freed at line %zu)", local->name,
             local->freed_loc.line);
    return;
  }
  local->freed = ctx->depth == 0 ? MEM_FREED_DEFINITE : MEM_FREED_MAYBE;
  local->freed_loc = loc;
}

/* Constant-size memory ops: { name, dest arg, size arg }. */
static const struct {
  const char *name;
  int dest_index;
  int size_index;
} MEM_OPS[] = {
    {"memcpy", 0, 2},  {"memmove", 0, 2},  {"memset", 0, 2},
    {"mem_copy", 0, 2}, {"mem_move", 0, 2}, {"mem_zero", 0, 1},
    {"mem_fill", 0, 2},
};

/* Capacity in bytes of the destination when it is frame memory the analysis
 * understands: `&arr[k]` with constant k, `&arr`, or a pointer local that
 * was assigned `&arr...` (offset unknown: full capacity, which still
 * catches sizes larger than the whole array). Returns 0 when unknown. */
static long long mem_dest_capacity(MemCtx *ctx, ASTNode *dest,
                                   const char **array_name_out) {
  dest = mem_unwrap_cast(dest);
  if (!dest) {
    return 0;
  }
  long long count = 0, elem_size = 0;
  if (dest->type == AST_UNARY_EXPRESSION) {
    UnaryExpression *unary = (UnaryExpression *)dest->data;
    if (!unary || !unary->operator || strcmp(unary->operator, "&") != 0) {
      return 0;
    }
    ASTNode *target = mem_unwrap_cast(unary->operand);
    if (target && target->type == AST_IDENTIFIER) {
      Identifier *id = (Identifier *)target->data;
      MemLocal *local = id ? mem_find_local(ctx, id->name) : NULL;
      if (local && local->is_stack &&
          mem_array_extent(ctx, local->type_name, &count, &elem_size)) {
        *array_name_out = local->name;
        return count * elem_size;
      }
      return 0;
    }
    if (target && target->type == AST_INDEX_EXPRESSION) {
      ArrayIndexExpression *index = (ArrayIndexExpression *)target->data;
      ASTNode *array = index ? mem_unwrap_cast(index->array) : NULL;
      if (!array || array->type != AST_IDENTIFIER) {
        return 0;
      }
      Identifier *id = (Identifier *)array->data;
      MemLocal *local = id ? mem_find_local(ctx, id->name) : NULL;
      long long offset = 0;
      if (local && local->is_stack &&
          mem_array_extent(ctx, local->type_name, &count, &elem_size) &&
          type_checker_eval_integer_constant_with_checker(
              ctx->checker, index->index, &offset) &&
          offset >= 0 && offset <= count) {
        *array_name_out = local->name;
        return (count - offset) * elem_size;
      }
      return 0;
    }
    return 0;
  }
  if (dest->type == AST_IDENTIFIER) {
    MemLocal *local = mem_expr_as_local(ctx, dest);
    if (local && local->points_to_stack) {
      MemLocal *target = mem_find_local(ctx, local->points_to_stack);
      if (target &&
          mem_array_extent(ctx, target->type_name, &count, &elem_size)) {
        *array_name_out = target->name;
        return count * elem_size;
      }
    }
  }
  return 0;
}

static void mem_check_mem_op(MemCtx *ctx, CallExpression *call,
                             SourceLocation loc) {
  for (size_t i = 0; i < sizeof(MEM_OPS) / sizeof(MEM_OPS[0]); i++) {
    if (strcmp(call->function_name, MEM_OPS[i].name) != 0) {
      continue;
    }
    if ((size_t)MEM_OPS[i].dest_index >= call->argument_count ||
        (size_t)MEM_OPS[i].size_index >= call->argument_count) {
      return;
    }
    long long size = 0;
    if (!type_checker_eval_integer_constant_with_checker(
            ctx->checker, call->arguments[MEM_OPS[i].size_index], &size) ||
        size <= 0) {
      return;
    }
    const char *array_name = NULL;
    long long capacity = mem_dest_capacity(
        ctx, call->arguments[MEM_OPS[i].dest_index], &array_name);
    if (capacity > 0 && size > capacity) {
      mem_error(ctx, loc,
                "Shrink the copy, or grow the destination array",
                "`%s` writes %lld bytes into `%s`, which only has %lld bytes "
                "left at this offset; this corrupts the stack frame",
                call->function_name, size, array_name, capacity);
    }
    return;
  }
}

static void mem_check_const_index(MemCtx *ctx, ASTNode *expr) {
  ArrayIndexExpression *index = (ArrayIndexExpression *)expr->data;
  ASTNode *array = index ? mem_unwrap_cast(index->array) : NULL;
  if (!array || array->type != AST_IDENTIFIER) {
    return;
  }
  Identifier *id = (Identifier *)array->data;
  MemLocal *local = id ? mem_find_local(ctx, id->name) : NULL;
  long long count = 0, elem_size = 0, value = 0;
  if (!local || !local->is_stack ||
      !mem_array_extent(ctx, local->type_name, &count, &elem_size) ||
      !type_checker_eval_integer_constant_with_checker(ctx->checker,
                                                       index->index, &value)) {
    return;
  }
  if (value < 0 || value >= count) {
    char suggestion[128];
    snprintf(suggestion, sizeof(suggestion),
             "Valid indexes for `%s` are 0..%lld", local->name, count - 1);
    mem_error(ctx, expr->location, suggestion,
              "Index %lld is out of bounds for `%s` (%s)", value, local->name,
              local->type_name);
  }
}

static void mem_walk_expr(MemCtx *ctx, ASTNode *expr) {
  if (!expr) {
    return;
  }
  switch (expr->type) {
  case AST_IDENTIFIER: {
    Identifier *id = (Identifier *)expr->data;
    mem_check_use(ctx, id ? mem_find_local(ctx, id->name) : NULL,
                  expr->location);
    return;
  }
  case AST_CAST_EXPRESSION: {
    CastExpression *cast = (CastExpression *)expr->data;
    mem_walk_expr(ctx, cast ? cast->operand : NULL);
    return;
  }
  case AST_UNARY_EXPRESSION: {
    UnaryExpression *unary = (UnaryExpression *)expr->data;
    if (!unary) {
      return;
    }
    if (unary->operator && strcmp(unary->operator, "&") == 0) {
      /* &p makes the pointer reachable elsewhere: it may be freed or kept
       * through the alias, so all definite knowledge about it ends here. */
      MemLocal *local = mem_expr_as_local(ctx, unary->operand);
      if (local) {
        local->escaped = 1;
        local->ever_freed = 1; /* could be freed through the alias */
      }
      return;
    }
    mem_walk_expr(ctx, unary->operand);
    return;
  }
  case AST_MEMBER_ACCESS: {
    MemberAccess *member = (MemberAccess *)expr->data;
    mem_walk_expr(ctx, member ? member->object : NULL);
    return;
  }
  case AST_INDEX_EXPRESSION: {
    ArrayIndexExpression *index = (ArrayIndexExpression *)expr->data;
    mem_check_const_index(ctx, expr);
    mem_walk_expr(ctx, index ? index->array : NULL);
    mem_walk_expr(ctx, index ? index->index : NULL);
    return;
  }
  case AST_BINARY_EXPRESSION: {
    BinaryExpression *binary = (BinaryExpression *)expr->data;
    if (binary) {
      mem_walk_expr(ctx, binary->left);
      mem_walk_expr(ctx, binary->right);
    }
    return;
  }
  case AST_FUNCTION_CALL: {
    CallExpression *call = (CallExpression *)expr->data;
    if (!call) {
      return;
    }
    if (call->object) {
      mem_walk_expr(ctx, call->object);
    }
    if (call->function_name && !call->object &&
        (strcmp(call->function_name, "free") == 0 ||
         strcmp(call->function_name, "realloc") == 0) &&
        call->argument_count >= 1) {
      /* The pointer argument is CONSUMED, not used; both invalidate it. */
      mem_handle_free(ctx, call->arguments[0], expr->location);
      for (size_t i = 1; i < call->argument_count; i++) {
        mem_walk_expr(ctx, call->arguments[i]);
      }
      return;
    }
    if (call->function_name && !call->object) {
      mem_check_mem_op(ctx, call, expr->location);
    }
    for (size_t i = 0; i < call->argument_count; i++) {
      mem_walk_expr(ctx, call->arguments[i]);
      /* A pointer handed to any call escapes the leak analysis: the callee
       * may keep or free it. */
      MemLocal *local = mem_expr_as_local(ctx, call->arguments[i]);
      if (local && local->is_pointer) {
        local->escaped = 1;
      }
    }
    return;
  }
  case AST_FUNC_PTR_CALL: {
    FuncPtrCall *call = (FuncPtrCall *)expr->data;
    if (!call) {
      return;
    }
    mem_walk_expr(ctx, call->function);
    for (size_t i = 0; i < call->argument_count; i++) {
      mem_walk_expr(ctx, call->arguments[i]);
      MemLocal *local = mem_expr_as_local(ctx, call->arguments[i]);
      if (local && local->is_pointer) {
        local->escaped = 1;
      }
    }
    return;
  }
  default:
    for (size_t i = 0; i < expr->child_count; i++) {
      mem_walk_expr(ctx, expr->children[i]);
    }
    return;
  }
}

/* ---- assignments and declarations -------------------------------------------- */

/* Apply `name = value` to the tracked state. */
static void mem_apply_assignment(MemCtx *ctx, const char *name, ASTNode *value,
                                 SourceLocation loc) {
  MemLocal *local = mem_find_local(ctx, name);
  if (local) {
    if (local->is_pointer) {
      local->freed = MEM_FREED_NO;
      local->holds_alloc = 0;
      local->points_to_stack = NULL;
      if (!ctx->in_defer && ctx->depth == 0 && mem_is_allocation(value)) {
        local->holds_alloc = 1;
        local->alloc_loc = loc;
        local->escaped = 0;
        local->ever_freed = 0;
      }
      MemLocal *stack_target = mem_addr_of_stack(ctx, value);
      if (stack_target) {
        local->points_to_stack = stack_target->name;
      }
      MemLocal *source = mem_expr_as_local(ctx, value);
      if (source && source->is_pointer) {
        /* aliasing: the allocation now has two names; stop tracking both */
        source->escaped = 1;
        local->points_to_stack = source->points_to_stack;
      }
    }
    return;
  }

  /* Not a local or parameter: a global. A stack address stored there
   * outlives the frame it points into. */
  MemLocal *stack_target = mem_addr_of_stack(ctx, value);
  if (stack_target) {
    mem_warn(ctx, loc,
             "Global `%s` is assigned the address of stack local `%s`; that "
             "address is dangling as soon as this function returns",
             name, stack_target->name);
  }
  MemLocal *source = mem_expr_as_local(ctx, value);
  if (source && source->is_pointer) {
    source->escaped = 1;
  }
}

/* ---- the statement walk --------------------------------------------------------- */

static void mem_walk_statement(MemCtx *ctx, ASTNode *statement);

static void mem_walk_block(MemCtx *ctx, ASTNode *block) {
  if (!block) {
    return;
  }
  if (block->type == AST_PROGRAM) {
    for (size_t i = 0; i < block->child_count; i++) {
      mem_walk_statement(ctx, block->children[i]);
    }
    return;
  }
  mem_walk_statement(ctx, block);
}

static void mem_walk_branch(MemCtx *ctx, ASTNode *body) {
  ctx->depth++;
  mem_walk_block(ctx, body);
  ctx->depth--;
}

static void mem_walk_statement(MemCtx *ctx, ASTNode *statement) {
  if (!statement) {
    return;
  }
  switch (statement->type) {
  case AST_VAR_DECLARATION: {
    VarDeclaration *decl = (VarDeclaration *)statement->data;
    if (!decl || !decl->name || !decl->type_name) {
      return;
    }
    if (decl->initializer) {
      mem_walk_expr(ctx, decl->initializer);
    }
    mem_add_local(ctx, decl->name, decl->type_name, statement->location, 0);
    if (decl->initializer) {
      mem_apply_assignment(ctx, decl->name, decl->initializer,
                           statement->location);
    }
    return;
  }
  case AST_ASSIGNMENT: {
    Assignment *assign = (Assignment *)statement->data;
    if (!assign) {
      return;
    }
    mem_walk_expr(ctx, assign->value);
    if (assign->target) {
      /* store through a field/index/deref: the value escapes */
      mem_walk_expr(ctx, assign->target);
      MemLocal *source = mem_expr_as_local(ctx, assign->value);
      if (source && source->is_pointer) {
        source->escaped = 1;
      }
      return;
    }
    if (assign->variable_name) {
      mem_apply_assignment(ctx, assign->variable_name, assign->value,
                           statement->location);
    }
    return;
  }
  case AST_RETURN_STATEMENT: {
    ReturnStatement *ret = (ReturnStatement *)statement->data;
    if (!ret || !ret->value) {
      return;
    }
    mem_walk_expr(ctx, ret->value);
    if (ctx->fn_returns_pointer) {
      MemLocal *stack_target = mem_addr_of_stack(ctx, ret->value);
      const char *via = NULL;
      if (!stack_target) {
        MemLocal *local = mem_expr_as_local(ctx, ret->value);
        if (local && local->points_to_stack) {
          stack_target = mem_find_local(ctx, local->points_to_stack);
          via = local->name;
        }
      }
      if (stack_target) {
        if (via) {
          mem_error(ctx, ret->value->location,
                    "Allocate the memory (`new` / `malloc`) or have the "
                    "caller pass a buffer in",
                    "Returning `%s`, which points at stack local `%s`; the "
                    "frame is destroyed when this function returns, so the "
                    "caller receives a dangling pointer",
                    via, stack_target->name);
        } else {
          mem_error(ctx, ret->value->location,
                    "Allocate the memory (`new` / `malloc`) or have the "
                    "caller pass a buffer in",
                    "Returning the address of stack local `%s`; the frame is "
                    "destroyed when this function returns, so the caller "
                    "receives a dangling pointer",
                    stack_target->name);
        }
      }
    }
    MemLocal *returned = mem_expr_as_local(ctx, ret->value);
    if (returned && returned->is_pointer) {
      returned->escaped = 1;
    }
    return;
  }
  case AST_IF_STATEMENT: {
    IfStatement *if_stmt = (IfStatement *)statement->data;
    if (!if_stmt) {
      return;
    }
    mem_walk_expr(ctx, if_stmt->condition);
    mem_walk_branch(ctx, if_stmt->then_branch);
    for (size_t i = 0; i < if_stmt->else_if_count; i++) {
      mem_walk_expr(ctx, if_stmt->else_ifs[i].condition);
      mem_walk_branch(ctx, if_stmt->else_ifs[i].body);
    }
    if (if_stmt->else_branch) {
      mem_walk_branch(ctx, if_stmt->else_branch);
    }
    return;
  }
  case AST_WHILE_STATEMENT: {
    WhileStatement *while_stmt = (WhileStatement *)statement->data;
    if (!while_stmt) {
      return;
    }
    mem_walk_expr(ctx, while_stmt->condition);
    mem_walk_branch(ctx, while_stmt->body);
    return;
  }
  case AST_FOR_STATEMENT: {
    ForStatement *for_stmt = (ForStatement *)statement->data;
    if (!for_stmt) {
      return;
    }
    if (for_stmt->initializer) {
      mem_walk_statement(ctx, for_stmt->initializer);
    }
    mem_walk_expr(ctx, for_stmt->condition);
    ctx->depth++;
    mem_walk_block(ctx, for_stmt->body);
    if (for_stmt->increment) {
      mem_walk_statement(ctx, for_stmt->increment);
    }
    ctx->depth--;
    return;
  }
  case AST_SWITCH_STATEMENT: {
    SwitchStatement *switch_stmt = (SwitchStatement *)statement->data;
    if (!switch_stmt) {
      return;
    }
    mem_walk_expr(ctx, switch_stmt->expression);
    for (size_t i = 0; i < switch_stmt->case_count; i++) {
      CaseClause *clause = switch_stmt->cases[i]
                               ? (CaseClause *)switch_stmt->cases[i]->data
                               : NULL;
      if (clause) {
        mem_walk_branch(ctx, clause->body);
      }
    }
    return;
  }
  case AST_MATCH_STATEMENT: {
    MatchStatement *match = (MatchStatement *)statement->data;
    if (!match) {
      return;
    }
    mem_walk_expr(ctx, match->expression);
    for (size_t i = 0; i < match->arm_count; i++) {
      mem_walk_branch(ctx, match->arms[i].body);
    }
    return;
  }
  case AST_DEFER_STATEMENT:
  case AST_ERRDEFER_STATEMENT: {
    DeferStatement *defer = (DeferStatement *)statement->data;
    if (!defer) {
      return;
    }
    int saved = ctx->in_defer;
    ctx->in_defer = 1;
    mem_walk_statement(ctx, defer->statement);
    ctx->in_defer = saved;
    return;
  }
  case AST_PROGRAM:
    mem_walk_block(ctx, statement);
    return;
  case AST_FUNCTION_CALL:
  case AST_FUNC_PTR_CALL:
    mem_walk_expr(ctx, statement);
    return;
  default:
    for (size_t i = 0; i < statement->child_count; i++) {
      mem_walk_statement(ctx, statement->children[i]);
    }
    return;
  }
}

/* ---- entry point ---------------------------------------------------------------- */

int type_checker_check_function_memory(TypeChecker *checker,
                                       ASTNode *declaration) {
  if (!checker || !checker->error_reporter || !declaration ||
      declaration->type != AST_FUNCTION_DECLARATION) {
    return 1;
  }
  FunctionDeclaration *fn = (FunctionDeclaration *)declaration->data;
  if (!fn || !fn->body || fn->is_extern) {
    return 1;
  }

  MemCtx ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.checker = checker;
  ctx.fn = fn;
  ctx.fn_loc = declaration->location;
  ctx.fn_returns_pointer = fn->return_type &&
                           mem_type_is_pointer(fn->return_type);

  for (size_t i = 0; i < fn->parameter_count; i++) {
    mem_add_local(&ctx, fn->parameter_names[i], fn->parameter_types[i],
                  declaration->location, 1);
  }

  mem_walk_block(&ctx, fn->body);

  /* Leaks: a spine allocation that was never freed (not even in a defer)
   * and never left the function has no owner when the function returns.
   * `main` is exempt: process exit reclaims everything, and warning about
   * it would train people to ignore this diagnostic. */
  if (strcmp(fn->name, "main") != 0) {
    for (size_t i = 0; i < ctx.local_count; i++) {
      MemLocal *local = &ctx.locals[i];
      if (local->holds_alloc && !local->ever_freed && !local->escaped) {
        mem_warn(&ctx, local->alloc_loc,
                 "`%s` is allocated here but never freed, returned, stored, "
                 "or passed on; the allocation leaks when `%s` returns",
                 local->name, fn->name);
      }
    }
  }

  return ctx.had_error ? 0 : 1;
}
