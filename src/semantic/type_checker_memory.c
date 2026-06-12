// Compile-time memory diagnostics: the analyses that catch the bugs people
// otherwise find at 2am with a hex dump.
//
// Two phases share one walker:
//
// PHASE 1 (per function, during declaration checking, scope still live):
//   M0101  use of a pointer after a direct free(p)              warning
//   M0102  double free (both frees direct)                     warning
//   M0103  returning the address of a stack local               ERROR
//   M0104  storing the address of a stack local in a global     warning
//   M0105  constant array index out of bounds (backstop)        ERROR
//   M0106  constant-size memory op overflowing a stack array    ERROR
//
// PHASE 2 (whole program, after every declaration type-checks):
//   Ownership summaries are INFERRED per function and iterated to fixpoint
//   over the call graph (imports are flattened, so the compiler sees the
//   whole program): which parameters a function definitely frees, which it
//   keeps a reference to, and whether its return value is a fresh
//   allocation the caller owns. The walker then re-runs with summaries:
//
//   M0108  use after a CALL freed the pointer (`consume(p); p[0]`) warning
//   M0109  double free where a call did one of the frees          warning
//   M0107  leak: an allocation that never escapes and is never
//          freed -- now seen THROUGH borrowing helpers, and fed by
//          wrapper allocators (`make() -> malloc(...)`)           warning
//
// The analysis is deliberately conservative: definite-bug states are only
// set on the function's straight-line spine, anything inside a branch or
// loop demotes to "maybe" and stays silent, and only DEFINITE summaries
// propagate across calls. A diagnostic from this file is meant to be
// trusted, so the false-positive budget is zero.

#include "type_checker_internal.h"

#define MEM_MAX_LOCALS 256
#define MEM_MAX_PARAMS 32
#define MEM_SUMMARY_MAX_ITER 8

typedef enum {
  MEM_FREED_NO = 0,
  MEM_FREED_DEFINITE = 1, /* freed on the spine: later use IS a bug */
  MEM_FREED_MAYBE = 2     /* freed inside a branch/loop: stay silent */
} MemFreedState;

typedef enum {
  MEM_MODE_LOCAL = 0,  /* phase 1: direct facts only; any call escapes */
  MEM_MODE_SUMMARY,    /* phase 2a: collect a function's summary, no reports */
  MEM_MODE_INTERPROC   /* phase 2b: report summary-dependent diagnostics */
} MemMode;

/* Inferred ownership facts about one function. Bit i refers to parameter i
 * (parameters past MEM_MAX_PARAMS are treated as stored). */
typedef struct {
  const char *name;
  FunctionDeclaration *fn; /* NULL for seeded externs (malloc, free, ...) */
  unsigned frees_definite; /* unconditionally frees param i (spine) */
  unsigned frees_maybe;    /* may free param i (branch, or maybe-callee) */
  unsigned stores;         /* keeps a reference to param i */
  int returns_fresh;       /* every returned value is a fresh allocation */
} MemFnSummary;

typedef struct {
  MemFnSummary *items;
  size_t count;
} MemSummaryTable;

typedef struct {
  const char *name;      /* AST-owned */
  const char *type_name; /* AST-owned */
  SourceLocation decl_loc;
  int param_index; /* -1 for locals */
  int is_stack;   /* array/struct/scalar local: its address dies with the frame */
  int is_pointer; /* trailing '*', cstring, or string (which carries a pointer) */
  int reassigned; /* a parameter overwritten since entry (summary collection) */
  MemFreedState freed;
  SourceLocation freed_loc;
  const char *freed_via; /* callee whose summary freed it; NULL = direct free */
  int holds_alloc; /* assigned from an allocator on the spine */
  SourceLocation alloc_loc;
  const char *alloc_via; /* wrapper allocator name; NULL = malloc/calloc/new */
  int escaped;     /* returned, stored, kept by a callee, or address taken */
  int ever_freed;  /* a free of this pointer appears ANYWHERE (defers count) */
  const char *points_to_stack; /* stack local whose address it holds, or NULL */
} MemLocal;

typedef struct {
  TypeChecker *checker;
  FunctionDeclaration *fn;
  SourceLocation fn_loc;
  MemMode mode;
  const MemSummaryTable *summaries; /* NULL in MEM_MODE_LOCAL */
  MemFnSummary *collect;            /* MEM_MODE_SUMMARY output */
  MemLocal locals[MEM_MAX_LOCALS];
  size_t local_count;
  int depth;      /* 0 = the function's straight-line spine */
  int in_defer;   /* defers run at scope exit: record facts, never report */
  int fn_returns_pointer;
  int saw_value_return;
  int returns_all_fresh;
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

/* ---- summary table ------------------------------------------------------------ */

static MemFnSummary *mem_summary_find(const MemSummaryTable *table,
                                      const char *name) {
  if (!table || !name) {
    return NULL;
  }
  for (size_t i = 0; i < table->count; i++) {
    if (strcmp(table->items[i].name, name) == 0) {
      return &table->items[i];
    }
  }
  return NULL;
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
                               int param_index) {
  if (!name || !type_name || ctx->local_count >= MEM_MAX_LOCALS) {
    return NULL;
  }
  MemLocal *local = &ctx->locals[ctx->local_count++];
  memset(local, 0, sizeof(*local));
  local->name = name;
  local->type_name = type_name;
  local->decl_loc = loc;
  local->param_index = param_index;
  local->is_pointer = mem_type_is_pointer(type_name);
  local->is_stack = param_index < 0 && !local->is_pointer &&
                    strncmp(type_name, "fn", 2) != 0;
  return local;
}

/* ---- diagnostics -------------------------------------------------------------- */

static void mem_warn(MemCtx *ctx, SourceLocation loc, const char *fmt, ...) {
  char message[512];
  va_list args;
  if (ctx->mode == MEM_MODE_SUMMARY) {
    return;
  }
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
  if (ctx->mode != MEM_MODE_LOCAL) {
    return;
  }
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

/* Fresh-allocation classification. Direct allocators always count; in the
 * summary/interproc modes a call to a function whose every return is fresh
 * counts too (the wrapper-allocator case). `via_out` names the wrapper. */
static int mem_is_allocation(MemCtx *ctx, ASTNode *expr, const char **via_out) {
  *via_out = NULL;
  expr = mem_unwrap_cast(expr);
  if (!expr) {
    return 0;
  }
  if (expr->type == AST_NEW_EXPRESSION) {
    return 1;
  }
  if (expr->type != AST_FUNCTION_CALL) {
    return 0;
  }
  CallExpression *call = (CallExpression *)expr->data;
  if (!call || !call->function_name || call->object) {
    return 0;
  }
  if (strcmp(call->function_name, "malloc") == 0 ||
      strcmp(call->function_name, "calloc") == 0 ||
      strcmp(call->function_name, "realloc") == 0) {
    return 1;
  }
  if (ctx->summaries) {
    MemFnSummary *summary = mem_summary_find(ctx->summaries,
                                             call->function_name);
    if (summary && summary->returns_fresh) {
      *via_out = summary->name;
      return 1;
    }
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
 * free()/realloc() and summary-freeing calls, marks escapes, and checks
 * constant-size memory ops against their destination's capacity. */

static void mem_walk_expr(MemCtx *ctx, ASTNode *expr);

static void mem_check_use(MemCtx *ctx, MemLocal *local, SourceLocation loc) {
  if (!local || ctx->in_defer || local->freed != MEM_FREED_DEFINITE) {
    return;
  }
  /* Phase split: direct-free bugs are phase 1's; bugs where a CALL did the
   * free are phase 2's (phase 1 cannot see them). */
  if (ctx->mode == MEM_MODE_LOCAL && local->freed_via == NULL) {
    mem_warn(ctx, loc,
             "Use of `%s` after it was freed (freed at line %zu); this is "
             "use-after-free",
             local->name, local->freed_loc.line);
    local->freed = MEM_FREED_MAYBE; /* one report per free site */
  } else if (ctx->mode == MEM_MODE_INTERPROC && local->freed_via != NULL) {
    mem_warn(ctx, loc,
             "Use of `%s` after the call to `%s` at line %zu freed it; this "
             "is use-after-free",
             local->name, local->freed_via, local->freed_loc.line);
    local->freed = MEM_FREED_MAYBE;
  }
}

/* A free event for `local`: a direct free()/realloc(), or (phase 2) a call
 * whose summary says the parameter is unconditionally freed. */
static void mem_free_event(MemCtx *ctx, MemLocal *local, SourceLocation loc,
                           const char *via) {
  if (!local || !local->is_pointer) {
    return;
  }
  local->ever_freed = 1;
  /* Summary: a free of an un-reassigned parameter is part of what this
   * function does to its caller's pointer. A `defer free(p)` counts -- it
   * runs unconditionally at scope exit -- so the `in_defer` flag does not
   * suppress summary recording (it only suppresses intra-function flow
   * events below). A spine free/defer is definite; a free inside a branch
   * is a maybe. */
  if (ctx->mode == MEM_MODE_SUMMARY && local->param_index >= 0 &&
      local->param_index < MEM_MAX_PARAMS && !local->reassigned &&
      ctx->collect) {
    if (ctx->depth == 0) {
      ctx->collect->frees_definite |= 1u << local->param_index;
    } else {
      ctx->collect->frees_maybe |= 1u << local->param_index;
    }
  }
  if (ctx->in_defer) {
    return; /* defers run at scope exit; their free is not a flow event */
  }
  if (local->freed == MEM_FREED_DEFINITE) {
    /* Double free. Phase 1 owns the both-direct case; phase 2 owns every
     * case where a call performed at least one of the frees. */
    int involves_call = local->freed_via != NULL || via != NULL;
    if (ctx->mode == MEM_MODE_LOCAL && !involves_call) {
      mem_warn(ctx, loc, "Double free of `%s` (already freed at line %zu)",
               local->name, local->freed_loc.line);
    } else if (ctx->mode == MEM_MODE_INTERPROC && involves_call) {
      if (via && local->freed_via) {
        mem_warn(ctx, loc,
                 "Double free of `%s`: the call to `%s` frees it, but the "
                 "call to `%s` at line %zu already did",
                 local->name, via, local->freed_via, local->freed_loc.line);
      } else if (via) {
        mem_warn(ctx, loc,
                 "Double free of `%s`: the call to `%s` frees it, but it was "
                 "already freed at line %zu",
                 local->name, via, local->freed_loc.line);
      } else {
        mem_warn(ctx, loc,
                 "Double free of `%s`: already freed by the call to `%s` at "
                 "line %zu",
                 local->name, local->freed_via, local->freed_loc.line);
      }
    }
    return;
  }
  local->freed = ctx->depth == 0 ? MEM_FREED_DEFINITE : MEM_FREED_MAYBE;
  local->freed_loc = loc;
  local->freed_via = via;
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
  if (ctx->mode != MEM_MODE_LOCAL) {
    return; /* phase 1 owns this diagnostic (function scope is live there) */
  }
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
  if (ctx->mode != MEM_MODE_LOCAL) {
    return; /* needs the live function scope for const locals */
  }
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

static void mem_collect_param_store(MemCtx *ctx, MemLocal *local) {
  if (ctx->mode == MEM_MODE_SUMMARY && local && local->param_index >= 0 &&
      local->param_index < MEM_MAX_PARAMS && !local->reassigned &&
      ctx->collect) {
    ctx->collect->stores |= 1u << local->param_index;
  }
}

static void mem_collect_param_maybe_free(MemCtx *ctx, MemLocal *local) {
  if (ctx->mode == MEM_MODE_SUMMARY && local && local->param_index >= 0 &&
      local->param_index < MEM_MAX_PARAMS && !local->reassigned &&
      ctx->collect) {
    ctx->collect->frees_maybe |= 1u << local->param_index;
  }
}

/* The ownership effect of passing `local` as argument `arg_index` of a call
 * to `callee`. With no summary (externs, indirect calls, methods, phase 1)
 * the pointer conservatively escapes. */
static void mem_apply_call_arg(MemCtx *ctx, MemLocal *local,
                               const char *callee, size_t arg_index,
                               SourceLocation loc) {
  if (!local || !local->is_pointer) {
    return;
  }
  MemFnSummary *summary =
      ctx->summaries && callee ? mem_summary_find(ctx->summaries, callee)
                               : NULL;
  if (!summary || arg_index >= MEM_MAX_PARAMS) {
    local->escaped = 1;
    mem_collect_param_store(ctx, local);
    return;
  }
  unsigned bit = 1u << arg_index;
  if (summary->frees_definite & bit) {
    mem_free_event(ctx, local, loc, summary->name);
    return;
  }
  if (summary->frees_maybe & bit) {
    /* might free: silence both the leak and any later use */
    local->ever_freed = 1;
    local->escaped = 1;
    mem_collect_param_maybe_free(ctx, local);
    return;
  }
  if (summary->stores & bit) {
    local->escaped = 1;
    mem_collect_param_store(ctx, local);
    return;
  }
  /* pure borrow: the callee looked at it and gave it back; keep tracking */
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
      mem_free_event(ctx, mem_expr_as_local(ctx, call->arguments[0]),
                     expr->location, NULL);
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
      mem_apply_call_arg(ctx, mem_expr_as_local(ctx, call->arguments[i]),
                         (!call->object && !call->is_indirect_call)
                             ? call->function_name
                             : NULL,
                         i, expr->location);
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
    if (local->param_index >= 0) {
      local->reassigned = 1;
    }
    if (local->is_pointer) {
      local->freed = MEM_FREED_NO;
      local->freed_via = NULL;
      local->holds_alloc = 0;
      local->points_to_stack = NULL;
      const char *via = NULL;
      if (!ctx->in_defer && ctx->depth == 0 &&
          mem_is_allocation(ctx, value, &via)) {
        local->holds_alloc = 1;
        local->alloc_loc = loc;
        local->alloc_via = via;
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
  if (stack_target && ctx->mode == MEM_MODE_LOCAL) {
    mem_warn(ctx, loc,
             "Global `%s` is assigned the address of stack local `%s`; that "
             "address is dangling as soon as this function returns",
             name, stack_target->name);
  }
  MemLocal *source = mem_expr_as_local(ctx, value);
  if (source && source->is_pointer) {
    source->escaped = 1;
    if (ctx->mode == MEM_MODE_SUMMARY && source->param_index >= 0 &&
        source->param_index < MEM_MAX_PARAMS && ctx->collect) {
      ctx->collect->stores |= 1u << source->param_index;
    }
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
    mem_add_local(ctx, decl->name, decl->type_name, statement->location, -1);
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
        if (ctx->mode == MEM_MODE_SUMMARY && source->param_index >= 0 &&
            source->param_index < MEM_MAX_PARAMS && ctx->collect) {
          ctx->collect->stores |= 1u << source->param_index;
        }
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
    if (ret->value) {
      ctx->saw_value_return = 1;
      /* Fresh if the value is an allocation expression, or a local that
       * holds one exclusively (the `var p = malloc(n); ...; return p;`
       * wrapper shape). A copy kept elsewhere disqualifies it: the caller
       * would not be the sole owner. */
      const char *via = NULL;
      MemLocal *returned_local = mem_expr_as_local(ctx, ret->value);
      int fresh = mem_is_allocation(ctx, ret->value, &via) ||
                  (returned_local && returned_local->holds_alloc &&
                   !returned_local->escaped);
      if (!fresh) {
        ctx->returns_all_fresh = 0;
      }
    }
    if (ctx->fn_returns_pointer && ctx->mode == MEM_MODE_LOCAL) {
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
      if (ctx->mode == MEM_MODE_SUMMARY && returned->param_index >= 0 &&
          returned->param_index < MEM_MAX_PARAMS && ctx->collect) {
        ctx->collect->stores |= 1u << returned->param_index;
      }
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

/* ---- shared walk setup ------------------------------------------------------------ */

static void mem_ctx_init(MemCtx *ctx, TypeChecker *checker, ASTNode *decl,
                         FunctionDeclaration *fn, MemMode mode,
                         const MemSummaryTable *summaries,
                         MemFnSummary *collect) {
  memset(ctx, 0, sizeof(*ctx));
  ctx->checker = checker;
  ctx->fn = fn;
  ctx->fn_loc = decl->location;
  ctx->mode = mode;
  ctx->summaries = summaries;
  ctx->collect = collect;
  ctx->returns_all_fresh = 1;
  ctx->fn_returns_pointer = fn->return_type &&
                            mem_type_is_pointer(fn->return_type);
  for (size_t i = 0; i < fn->parameter_count; i++) {
    mem_add_local(ctx, fn->parameter_names[i], fn->parameter_types[i],
                  decl->location, (int)i);
  }
}

/* ---- phase 1 entry point ------------------------------------------------------------ */

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
  mem_ctx_init(&ctx, checker, declaration, fn, MEM_MODE_LOCAL, NULL, NULL);
  mem_walk_block(&ctx, fn->body);
  return ctx.had_error ? 0 : 1;
}

/* ---- phase 2: whole-program ownership inference ------------------------------------- */

static int mem_decl_is_analyzable(ASTNode *decl) {
  if (!decl || decl->type != AST_FUNCTION_DECLARATION) {
    return 0;
  }
  FunctionDeclaration *fn = (FunctionDeclaration *)decl->data;
  return fn && fn->body && !fn->is_extern && fn->type_param_count == 0 &&
         fn->name;
}

/* One summary-collection walk of `decl`; returns 1 when the recorded facts
 * changed (drives the fixpoint). */
static int mem_collect_summary(TypeChecker *checker, ASTNode *decl,
                               MemSummaryTable *table, MemFnSummary *summary) {
  FunctionDeclaration *fn = (FunctionDeclaration *)decl->data;
  MemFnSummary before = *summary;

  MemCtx ctx;
  mem_ctx_init(&ctx, checker, decl, fn, MEM_MODE_SUMMARY, table, summary);
  mem_walk_block(&ctx, fn->body);

  /* returns_fresh: a pointer-returning function whose every value-return is
   * a fresh allocation behaves like malloc for its callers. */
  int fresh = ctx.fn_returns_pointer && ctx.saw_value_return &&
              ctx.returns_all_fresh;
  summary->returns_fresh |= fresh; /* monotone: only ever turns on */

  return summary->frees_definite != before.frees_definite ||
         summary->frees_maybe != before.frees_maybe ||
         summary->stores != before.stores ||
         summary->returns_fresh != before.returns_fresh;
}

int type_checker_check_program_memory(TypeChecker *checker, ASTNode *program) {
  if (!checker || !checker->error_reporter || !program ||
      program->type != AST_PROGRAM || checker->has_error) {
    return 1;
  }
  Program *prog = (Program *)program->data;
  if (!prog) {
    return 1;
  }

  /* Seed the table with the C allocator externs, then one slot per
   * analyzable function. */
  size_t capacity = prog->declaration_count + 4;
  MemFnSummary *items = calloc(capacity, sizeof(MemFnSummary));
  ASTNode **decls = calloc(capacity, sizeof(ASTNode *));
  if (!items || !decls) {
    free(items);
    free(decls);
    return 1;
  }
  MemSummaryTable table = {items, 0};

  items[table.count++] = (MemFnSummary){"free", NULL, 1u, 0, 0, 0};
  items[table.count++] = (MemFnSummary){"realloc", NULL, 1u, 0, 0, 1};
  items[table.count++] = (MemFnSummary){"malloc", NULL, 0, 0, 0, 1};
  items[table.count++] = (MemFnSummary){"calloc", NULL, 0, 0, 0, 1};

  for (size_t i = 0; i < prog->declaration_count; i++) {
    if (!mem_decl_is_analyzable(prog->declarations[i]) ||
        table.count >= capacity) {
      continue;
    }
    FunctionDeclaration *fn =
        (FunctionDeclaration *)prog->declarations[i]->data;
    if (mem_summary_find(&table, fn->name)) {
      continue; /* duplicate name: first definition wins */
    }
    decls[table.count] = prog->declarations[i];
    items[table.count] = (MemFnSummary){fn->name, fn, 0, 0, 0, 0};
    table.count++;
  }

  /* Fixpoint: facts are monotone (bits only get set), so this terminates;
   * the iteration cap is belt-and-braces. */
  for (int iteration = 0; iteration < MEM_SUMMARY_MAX_ITER; iteration++) {
    int changed = 0;
    for (size_t i = 0; i < table.count; i++) {
      if (decls[i]) {
        changed |= mem_collect_summary(checker, decls[i], &table, &items[i]);
      }
    }
    if (!changed) {
      break;
    }
  }

  /* Reporting pass: summary-aware leaks and cross-call use-after-free. */
  for (size_t i = 0; i < table.count; i++) {
    if (!decls[i]) {
      continue;
    }
    FunctionDeclaration *fn = (FunctionDeclaration *)decls[i]->data;
    MemCtx ctx;
    mem_ctx_init(&ctx, checker, decls[i], fn, MEM_MODE_INTERPROC, &table,
                 NULL);
    mem_walk_block(&ctx, fn->body);

    /* Leaks: a spine allocation that was never freed (not even in a defer)
     * and never left the function has no owner when the function returns.
     * `main` is exempt: process exit reclaims everything, and warning
     * about it would train people to ignore this diagnostic. */
    if (strcmp(fn->name, "main") != 0) {
      for (size_t j = 0; j < ctx.local_count; j++) {
        MemLocal *local = &ctx.locals[j];
        if (!local->holds_alloc || local->ever_freed || local->escaped) {
          continue;
        }
        if (local->alloc_via) {
          mem_warn(&ctx, local->alloc_loc,
                   "`%s` holds the allocation `%s` returns, but it is never "
                   "freed, returned, stored, or passed on; the allocation "
                   "leaks when `%s` returns",
                   local->name, local->alloc_via, fn->name);
        } else {
          mem_warn(&ctx, local->alloc_loc,
                   "`%s` is allocated here but never freed, returned, "
                   "stored, or passed on; the allocation leaks when `%s` "
                   "returns",
                   local->name, fn->name);
        }
      }
    }
  }

  free(items);
  free(decls);
  return 1;
}
