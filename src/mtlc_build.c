/* mtlc_build.c - implementation of the public IR builder (include/mtlc/build.h).
 *
 * Part of libmtlc. This is the frontend-agnostic path for CONSTRUCTING IR: it
 * turns the opaque builder calls into a backend IRProgram, then populates the
 * module type registry and symbol table the code generators read -- doing, for
 * an arbitrary frontend, exactly what src/frontend/mtlc_lower_module.c does for
 * the reference Mettle frontend, but from the builder's own declared types
 * instead of a frontend AST. It is frontend-free: it includes only the public
 * mtlc/ headers and the backend IR. */
#include "mtlc/build.h"
#include "mtlc/module.h"

#include "common.h"
#include "ir/ir.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A function declaration recorded for the module symbol table. Bodies (when not
 * extern) are emitted directly into `ir`. */
typedef struct {
  char *name;
  const MtlcType *return_type;
  char **param_names;         /* owned copies */
  const MtlcType **param_types; /* borrowed (immortal singletons) */
  size_t param_count;
  int is_extern;
  int has_body;
  int is_kernel;
} FnDecl;

struct MtlcFn {
  IRFunction *ir;      /* borrowed; owned by the program */
  MtlcBuilder *builder;
  const MtlcType *return_type;
  char **param_names;  /* borrowed from the FnDecl */
  const MtlcType **param_types; /* borrowed from the FnDecl */
  size_t param_count;
  IROperand *values;   /* value-handle table; each owns its operand */
  size_t value_count, value_capacity;
  /* Labels allocated by mtlc_label_new, so a double placement is caught here
   * and an unplaced-but-branched-to label is caught at finish. */
  char **labels;
  unsigned char *label_placed;
  size_t label_count, label_capacity;
};

/* A module-level global variable declaration. */
typedef struct {
  char *name;
  const MtlcType *type;
  long long init_value;
  int is_extern;
} GlobalDecl;

struct MtlcBuilder {
  IRProgram *program;  /* owned until finish */
  MtlcFn **fns;        /* body builders (non-extern), owned */
  size_t fn_count, fn_capacity;
  FnDecl *decls;       /* every declaration, owned */
  size_t decl_count, decl_capacity;
  GlobalDecl *globals; /* module globals, owned */
  size_t global_count, global_capacity;
  /* every distinct MtlcType* that passed through the builder API, registered
   * by name into the module type registry at finish so codegen can resolve
   * any type NAME the IR carries (parameter types, DECLARE_LOCAL/CAST text) */
  const MtlcType **seen_types;
  size_t seen_count, seen_capacity;
  int temp_counter;
  int label_counter;
  int error;
  char error_message[512];
  MtlcDiagHandler diag_handler;
  void *diag_user_data;
};

/* Latch the first construction error and report it. Errors are sticky: once
 * one lands every later builder call is a no-op, so a frontend can emit a whole
 * function and test once, and the message still names the call that broke. */
static void builder_fail_impl(MtlcBuilder *b, const char *api,
                              const char *format, ...) {
  char detail[384];
  va_list args;
  va_start(args, format);
  vsnprintf(detail, sizeof(detail), format, args);
  va_end(args);

  char message[512];
  snprintf(message, sizeof(message), "%s: %s", api ? api : "mtlc_builder",
           detail);
  if (!b) {
    return;
  }
  if (!b->error) {
    b->error = 1;
    snprintf(b->error_message, sizeof(b->error_message), "%s", message);
  }
  if (b->diag_handler) {
    b->diag_handler(b->diag_user_data, MTLC_DIAG_ERROR, message);
  }
}

#define BUILDER_FAIL(b, ...) builder_fail_impl((b), __func__, __VA_ARGS__)

/* The parseable NAME of a type: its canonical name when set (scalars carry
 * "int64", interned pointers carry "int64*"), else the kind name. */
static const char *type_name(const MtlcType *t) {
  if (!t) {
    return "int64";
  }
  return t->name ? t->name : mtlc_type_kind_name(t->kind);
}

static void record_type(MtlcBuilder *b, const MtlcType *t) {
  if (!b || !t) {
    return;
  }
  for (size_t i = 0; i < b->seen_count; i++) {
    if (b->seen_types[i] == t) {
      return;
    }
  }
  if (b->seen_count == b->seen_capacity) {
    size_t next = b->seen_capacity ? b->seen_capacity * 2 : 16;
    const MtlcType **grown =
        realloc(b->seen_types, next * sizeof(*b->seen_types));
    if (!grown) {
      BUILDER_FAIL(b, "invalid argument or allocation failure");
      return;
    }
    b->seen_types = grown;
    b->seen_capacity = next;
  }
  b->seen_types[b->seen_count++] = t;
  /* pointer chains: seeing "int64*" implies "int64" should resolve too */
  if (t->base_type) {
    record_type(b, t->base_type);
  }
}

/* ------------------------------------------------------------------ builder */

MtlcBuilder *mtlc_builder_create(void) {
  MtlcBuilder *b = (MtlcBuilder *)calloc(1, sizeof(MtlcBuilder));
  if (!b) {
    return NULL;
  }
  b->program = ir_program_create();
  if (!b->program) {
    free(b);
    return NULL;
  }
  return b;
}

static void fn_destroy(MtlcFn *fn) {
  if (!fn) {
    return;
  }
  for (size_t i = 0; i < fn->value_count; i++) {
    ir_operand_destroy(&fn->values[i]);
  }
  free(fn->values);
  for (size_t i = 0; i < fn->label_count; i++) {
    free(fn->labels[i]);
  }
  free(fn->labels);
  free(fn->label_placed);
  free(fn);
}

void mtlc_builder_destroy(MtlcBuilder *builder) {
  if (!builder) {
    return;
  }
  for (size_t i = 0; i < builder->fn_count; i++) {
    fn_destroy(builder->fns[i]);
  }
  free(builder->fns);
  for (size_t i = 0; i < builder->decl_count; i++) {
    free(builder->decls[i].name);
    for (size_t p = 0; p < builder->decls[i].param_count; p++) {
      free(builder->decls[i].param_names[p]);
    }
    free(builder->decls[i].param_names);
    free(builder->decls[i].param_types);
  }
  free(builder->decls);
  for (size_t i = 0; i < builder->global_count; i++) {
    free(builder->globals[i].name);
  }
  free(builder->globals);
  free(builder->seen_types);
  if (builder->program) {
    ir_program_destroy(builder->program);
  }
  free(builder);
}

void mtlc_builder_set_diagnostic_handler(MtlcBuilder *builder,
                                         MtlcDiagHandler handler,
                                         void *user_data) {
  if (builder) {
    builder->diag_handler = handler;
    builder->diag_user_data = user_data;
  }
}

int mtlc_builder_ok(const MtlcBuilder *builder) {
  return builder && !builder->error;
}

const char *mtlc_builder_error(const MtlcBuilder *builder) {
  if (!builder || !builder->error) {
    return NULL;
  }
  return builder->error_message[0] ? builder->error_message
                                   : "mtlc_builder: construction failed";
}

int mtlc_fn_ok(const MtlcFn *fn) {
  return fn && fn->builder && !fn->builder->error;
}

void mtlc_builder_global(MtlcBuilder *builder, const char *name,
                        const MtlcType *type, long long init_value,
                        int is_extern) {
  if (!builder || builder->error || !name || !type) {
    if (builder) {
      BUILDER_FAIL(builder, "invalid argument or allocation failure");
    }
    return;
  }
  if (builder->global_count == builder->global_capacity) {
    size_t next = builder->global_capacity ? builder->global_capacity * 2 : 8;
    GlobalDecl *grown = realloc(builder->globals, next * sizeof(GlobalDecl));
    if (!grown) {
      BUILDER_FAIL(builder, "invalid argument or allocation failure");
      return;
    }
    builder->globals = grown;
    builder->global_capacity = next;
  }
  GlobalDecl *g = &builder->globals[builder->global_count++];
  g->name = mettle_strdup(name);
  g->type = type;
  g->init_value = init_value;
  g->is_extern = is_extern ? 1 : 0;
  record_type(builder, type);
}

static int kernel_scalar_type(const MtlcType *type) {
  return type &&
         (mtlc_type_is_integer(type) || mtlc_type_is_float(type) ||
          type->kind == MTLC_TYPE_BOOL);
}

static int kernel_parameter_type(const MtlcType *type) {
  return kernel_scalar_type(type) ||
         (type && type->kind == MTLC_TYPE_POINTER && type->base_type &&
          kernel_scalar_type(type->base_type));
}

static MtlcFn *builder_function_impl(MtlcBuilder *builder, const char *name,
                                    const MtlcType *return_type,
                                    const char *const *param_names,
                                    const MtlcType *const *param_types,
                                    size_t param_count, int is_extern,
                                    int is_kernel) {
  if (!builder || builder->error || !name || !return_type) {
    return NULL;
  }
  if (is_kernel) {
    if (is_extern || return_type->kind != MTLC_TYPE_VOID) {
      BUILDER_FAIL(builder, "invalid argument or allocation failure");
      return NULL;
    }
    for (size_t i = 0; i < param_count; i++) {
      if (!param_types || !kernel_parameter_type(param_types[i])) {
        BUILDER_FAIL(builder, "invalid argument or allocation failure");
        return NULL;
      }
    }
  }

  /* record the declaration (used at finish for the module symbol table) */
  if (builder->decl_count == builder->decl_capacity) {
    size_t next = builder->decl_capacity ? builder->decl_capacity * 2 : 8;
    FnDecl *grown = realloc(builder->decls, next * sizeof(FnDecl));
    if (!grown) {
      BUILDER_FAIL(builder, "invalid argument or allocation failure");
      return NULL;
    }
    builder->decls = grown;
    builder->decl_capacity = next;
  }
  FnDecl *decl = &builder->decls[builder->decl_count];
  memset(decl, 0, sizeof(*decl));
  decl->name = mettle_strdup(name);
  decl->return_type = return_type;
  decl->param_count = param_count;
  decl->is_extern = is_extern ? 1 : 0;
  decl->has_body = is_extern ? 0 : 1;
  decl->is_kernel = is_kernel ? 1 : 0;
  if (param_count > 0) {
    decl->param_names = calloc(param_count, sizeof(char *));
    decl->param_types = calloc(param_count, sizeof(MtlcType *));
    for (size_t i = 0; i < param_count; i++) {
      decl->param_names[i] =
          mettle_strdup(param_names && param_names[i] ? param_names[i] : "");
      decl->param_types[i] = param_types ? param_types[i] : NULL;
    }
  }
  builder->decl_count++;

  if (is_extern) {
    return NULL; /* body-less: nothing to emit into */
  }

  /* create the IR function and set its signature by type NAME (resolved against
   * the type registry at codegen time) */
  IRFunction *irf = ir_function_create(name);
  if (!irf) {
    BUILDER_FAIL(builder, "invalid argument or allocation failure");
    return NULL;
  }
  if (param_count > 0) {
    const char **type_names = calloc(param_count, sizeof(char *));
    for (size_t i = 0; i < param_count; i++) {
      type_names[i] = type_name(param_types ? param_types[i] : NULL);
      if (param_types && param_types[i]) {
        record_type(builder, param_types[i]);
      }
    }
    ir_function_set_parameters(irf, (const char **)decl->param_names,
                               type_names, param_count);
    free(type_names);
  }
  irf->return_type_name = mettle_strdup(type_name(return_type));
  irf->is_kernel = is_kernel ? 1 : 0;
  record_type(builder, return_type);
  if (!ir_program_add_function(builder->program, irf)) {
    ir_function_destroy(irf);
    BUILDER_FAIL(builder, "invalid argument or allocation failure");
    return NULL;
  }

  MtlcFn *fn = calloc(1, sizeof(MtlcFn));
  if (!fn) {
    BUILDER_FAIL(builder, "invalid argument or allocation failure");
    return NULL;
  }
  fn->ir = irf;
  fn->builder = builder;
  fn->return_type = return_type;
  fn->param_names = decl->param_names;
  fn->param_types = decl->param_types;
  fn->param_count = param_count;

  if (builder->fn_count == builder->fn_capacity) {
    size_t next = builder->fn_capacity ? builder->fn_capacity * 2 : 8;
    MtlcFn **grown = realloc(builder->fns, next * sizeof(MtlcFn *));
    if (!grown) {
      fn_destroy(fn);
      BUILDER_FAIL(builder, "invalid argument or allocation failure");
      return NULL;
    }
    builder->fns = grown;
    builder->fn_capacity = next;
  }
  builder->fns[builder->fn_count++] = fn;
  return fn;
}

MtlcFn *mtlc_builder_function(MtlcBuilder *builder, const char *name,
                              const MtlcType *return_type,
                              const char *const *param_names,
                              const MtlcType *const *param_types,
                              size_t param_count, int is_extern) {
  return builder_function_impl(builder, name, return_type, param_names,
                               param_types, param_count, is_extern, 0);
}

int mtlc_builder_declare_function(MtlcBuilder *builder, const char *name,
                                  const MtlcType *return_type,
                                  const char *const *param_names,
                                  const MtlcType *const *param_types,
                                  size_t param_count) {
  if (!builder) {
    return 0;
  }
  int was_ok = !builder->error;
  builder_function_impl(builder, name, return_type, param_names, param_types,
                        param_count, /*is_extern=*/1, /*is_kernel=*/0);
  /* An extern declaration has no body builder, so NULL is its success value
   * too; the error latch is what actually distinguishes the two. */
  return was_ok && !builder->error;
}

MtlcFn *mtlc_builder_kernel(MtlcBuilder *builder, const char *name,
                            const char *const *param_names,
                            const MtlcType *const *param_types,
                            size_t param_count) {
  return builder_function_impl(
      builder, name, mtlc_type_scalar(MTLC_TYPE_VOID), param_names, param_types,
      param_count, 0, 1);
}

/* ------------------------------------------------------------------- values */

/* IEEE width (0/32/64) of a type, 0 when not floating. */
static int float_bits_of(const MtlcType *t) {
  if (!t || !mtlc_type_is_float(t)) {
    return 0;
  }
  return (t->kind == MTLC_TYPE_FLOAT32) ? 32 : 64;
}

/* Record a float value's width on its handle, so a later use site (CAST in
 * particular) can tell what it is holding. The operand is copied into every
 * instruction that uses the handle, so codegen's operand_float_bits sees the
 * width too. */
static void value_set_float_bits(MtlcFn *fn, MtlcValue v, int bits) {
  if (bits && v >= 0 && (size_t)v < fn->value_count) {
    fn->values[v].float_bits = bits;
  }
}

static MtlcValue push_value(MtlcFn *fn, IROperand op) {
  if (fn->value_count == fn->value_capacity) {
    size_t next = fn->value_capacity ? fn->value_capacity * 2 : 16;
    IROperand *grown = realloc(fn->values, next * sizeof(IROperand));
    if (!grown) {
      ir_operand_destroy(&op);
      BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
      return MTLC_NO_VALUE;
    }
    fn->values = grown;
    fn->value_capacity = next;
  }
  fn->values[fn->value_count] = op; /* takes ownership of op's name */
  return (MtlcValue)fn->value_count++;
}

static const IROperand *value_operand(MtlcFn *fn, MtlcValue v) {
  if (v < 0 || (size_t)v >= fn->value_count) {
    return NULL;
  }
  return &fn->values[v];
}

/* Append a stack-built instruction. Its dest/lhs/rhs/arguments operands are
 * shallow copies that ALIAS handle-table entries (or borrowed literals); the
 * append clones every operand and strdup's the text, so the stack instruction
 * is discarded without a destroy. */
static void emit(MtlcFn *fn, const IRInstruction *inst) {
  if (fn->builder->error) {
    return;
  }
  if (!ir_function_append_instruction(fn->ir, inst)) {
    BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
  }
}

static MtlcValue fresh_temp(MtlcFn *fn) {
  char name[32];
  snprintf(name, sizeof(name), ".t%d", fn->builder->temp_counter++);
  return push_value(fn, ir_operand_temp(name));
}

MtlcValue mtlc_fn_param(MtlcFn *fn, size_t index) {
  if (!fn || index >= fn->param_count) {
    if (fn) {
      BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
    }
    return MTLC_NO_VALUE;
  }
  MtlcValue res = push_value(fn, ir_operand_symbol(fn->param_names[index]));
  if (fn->param_types) {
    value_set_float_bits(fn, res, float_bits_of(fn->param_types[index]));
  }
  return res;
}

MtlcValue mtlc_const_int(MtlcFn *fn, const MtlcType *type, long long value) {
  (void)type; /* an int literal carries its width at its use site */
  if (!fn) {
    return MTLC_NO_VALUE;
  }
  return push_value(fn, ir_operand_int(value));
}

MtlcValue mtlc_local(MtlcFn *fn, const char *name, const MtlcType *type) {
  if (!fn || !name || !type) {
    if (fn) {
      BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
    }
    return MTLC_NO_VALUE;
  }
  record_type(fn->builder, type);
  MtlcValue res = push_value(fn, ir_operand_symbol(name));
  value_set_float_bits(fn, res, float_bits_of(type));
  const IROperand *dest = value_operand(fn, res);
  if (!dest) {
    return MTLC_NO_VALUE;
  }
  IRInstruction inst = {0};
  inst.op = IR_OP_DECLARE_LOCAL;
  inst.dest = *dest;
  inst.text = (char *)type_name(type);
  inst.value_type = (MtlcType *)type;
  emit(fn, &inst);
  return res;
}

MtlcValue mtlc_global_ref(MtlcFn *fn, const char *name) {
  if (!fn || !name) {
    if (fn) {
      BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
    }
    return MTLC_NO_VALUE;
  }
  MtlcValue res = push_value(fn, ir_operand_symbol(name));
  for (size_t i = 0; i < fn->builder->global_count; i++) {
    if (strcmp(fn->builder->globals[i].name, name) == 0) {
      value_set_float_bits(fn, res, float_bits_of(fn->builder->globals[i].type));
      break;
    }
  }
  return res;
}

MtlcValue mtlc_const_float(MtlcFn *fn, const MtlcType *type, double value) {
  if (!fn || !type) {
    if (fn) {
      BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
    }
    return MTLC_NO_VALUE;
  }
  int bits = (type->kind == MTLC_TYPE_FLOAT32) ? 32 : 64;
  return push_value(fn, ir_operand_float_sized(value, bits));
}

/* --------------------------------------------------------------- instructions */

void mtlc_assign(MtlcFn *fn, MtlcValue dest, MtlcValue value) {
  if (!fn) {
    return;
  }
  const IROperand *d = value_operand(fn, dest);
  const IROperand *v = value_operand(fn, value);
  if (!d || !v) {
    BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
    return;
  }
  IRInstruction inst = {0};
  inst.op = IR_OP_ASSIGN;
  inst.dest = *d;
  inst.lhs = *v;
  emit(fn, &inst);
}

const char *mtlc_binary_op_name(MtlcBinaryOp op) {
  switch (op) {
  case MTLC_BINOP_ADD: return "+";
  case MTLC_BINOP_SUB: return "-";
  case MTLC_BINOP_MUL: return "*";
  case MTLC_BINOP_DIV: return "/";
  case MTLC_BINOP_REM: return "%";
  case MTLC_BINOP_EQ: return "==";
  case MTLC_BINOP_NE: return "!=";
  case MTLC_BINOP_LT: return "<";
  case MTLC_BINOP_LE: return "<=";
  case MTLC_BINOP_GT: return ">";
  case MTLC_BINOP_GE: return ">=";
  case MTLC_BINOP_LOGICAL_AND: return "&&";
  case MTLC_BINOP_LOGICAL_OR: return "||";
  case MTLC_BINOP_BIT_AND: return "&";
  case MTLC_BINOP_BIT_OR: return "|";
  case MTLC_BINOP_BIT_XOR: return "^";
  case MTLC_BINOP_SHL: return "<<";
  case MTLC_BINOP_SHR: return ">>";
  }
  return NULL;
}

const char *mtlc_unary_op_name(MtlcUnaryOp op) {
  switch (op) {
  case MTLC_UNOP_NEGATE: return "-";
  case MTLC_UNOP_LOGICAL_NOT: return "!";
  case MTLC_UNOP_BITWISE_NOT: return "~";
  }
  return NULL;
}

/* The instruction text must outlive the call, and ir_function_append_instruction
 * copies it -- but only the canonical spellings below are ever accepted, so the
 * pointer handed to the IR is always a string literal either way. */
static const char *canonical_binary_op(const char *op) {
  static const char *const ops[] = {"+",  "-",  "*",  "/",  "%",  "==", "!=",
                                    "<",  "<=", ">",  ">=", "&&", "||", "&",
                                    "|",  "^",  "<<", ">>"};
  for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
    if (strcmp(op, ops[i]) == 0) {
      return ops[i];
    }
  }
  return NULL;
}

static const char *canonical_unary_op(const char *op) {
  static const char *const ops[] = {"-", "!", "~"};
  for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
    if (strcmp(op, ops[i]) == 0) {
      return ops[i];
    }
  }
  return NULL;
}

MtlcValue mtlc_binary_op(MtlcFn *fn, MtlcBinaryOp op, MtlcValue lhs,
                         MtlcValue rhs, const MtlcType *result_type) {
  const char *text = mtlc_binary_op_name(op);
  if (!text) {
    if (fn) {
      BUILDER_FAIL(fn->builder, "unknown binary operator %d", (int)op);
    }
    return MTLC_NO_VALUE;
  }
  return mtlc_binary(fn, text, lhs, rhs, result_type);
}

MtlcValue mtlc_unary_op(MtlcFn *fn, MtlcUnaryOp op, MtlcValue operand,
                        const MtlcType *result_type) {
  const char *text = mtlc_unary_op_name(op);
  if (!text) {
    if (fn) {
      BUILDER_FAIL(fn->builder, "unknown unary operator %d", (int)op);
    }
    return MTLC_NO_VALUE;
  }
  return mtlc_unary(fn, text, operand, result_type);
}

MtlcValue mtlc_binary(MtlcFn *fn, const char *op, MtlcValue lhs, MtlcValue rhs,
                     const MtlcType *result_type) {
  if (!fn || !op || !result_type) {
    if (fn) {
      BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
    }
    return MTLC_NO_VALUE;
  }
  const char *requested = op;
  op = canonical_binary_op(op);
  if (!op) {
    BUILDER_FAIL(fn->builder, "'%s' is not a binary operator", requested);
    return MTLC_NO_VALUE;
  }
  const IROperand *l = value_operand(fn, lhs);
  const IROperand *r = value_operand(fn, rhs);
  if (!l || !r) {
    BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
    return MTLC_NO_VALUE;
  }
  record_type(fn->builder, result_type);
  /* copy operands before push_value may realloc the table out from under them */
  IROperand lc = *l, rc = *r;
  MtlcValue res = fresh_temp(fn);
  const IROperand *dest = value_operand(fn, res);
  if (!dest) {
    return MTLC_NO_VALUE;
  }
  IRInstruction inst = {0};
  inst.op = IR_OP_BINARY;
  inst.dest = *dest;
  inst.lhs = lc;
  inst.rhs = rc;
  inst.text = (char *)op;
  inst.value_type = (MtlcType *)result_type;
  if (mtlc_type_is_float(result_type)) {
    inst.is_float = 1;
    inst.float_bits = (int)(result_type->size * 8);
    /* Only arithmetic yields a float VALUE; a float comparison is flagged
     * is_float (so codegen picks ucomis) but produces an integer 0/1. */
    if (strcmp(op, "+") == 0 || strcmp(op, "-") == 0 ||
        strcmp(op, "*") == 0 || strcmp(op, "/") == 0) {
      value_set_float_bits(fn, res, inst.float_bits);
    }
  } else if (mtlc_type_is_unsigned(result_type)) {
    /* unsigned result type selects unsigned / % >> and compares */
    inst.is_unsigned = 1;
  }
  emit(fn, &inst);
  return res;
}

MtlcValue mtlc_unary(MtlcFn *fn, const char *op, MtlcValue operand,
                    const MtlcType *result_type) {
  if (!fn || !op || !result_type) {
    if (fn) {
      BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
    }
    return MTLC_NO_VALUE;
  }
  const char *requested = op;
  op = canonical_unary_op(op);
  if (!op) {
    BUILDER_FAIL(fn->builder, "'%s' is not a unary operator", requested);
    return MTLC_NO_VALUE;
  }
  const IROperand *o = value_operand(fn, operand);
  if (!o) {
    BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
    return MTLC_NO_VALUE;
  }
  IROperand oc = *o;
  MtlcValue res = fresh_temp(fn);
  const IROperand *dest = value_operand(fn, res);
  if (!dest) {
    return MTLC_NO_VALUE;
  }
  IRInstruction inst = {0};
  inst.op = IR_OP_UNARY;
  inst.dest = *dest;
  inst.lhs = oc;
  inst.text = (char *)op;
  inst.value_type = (MtlcType *)result_type;
  if (mtlc_type_is_float(result_type)) {
    inst.is_float = 1;
    inst.float_bits = (int)(result_type->size * 8);
    if (strcmp(op, "+") == 0 || strcmp(op, "-") == 0) {
      value_set_float_bits(fn, res, inst.float_bits);
    }
  }
  emit(fn, &inst);
  return res;
}

MtlcValue mtlc_call(MtlcFn *fn, const char *callee, const MtlcValue *args,
                   size_t arg_count, const MtlcType *return_type) {
  if (!fn || !callee || !return_type) {
    if (fn) {
      BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
    }
    return MTLC_NO_VALUE;
  }
  IROperand *argv = NULL;
  if (arg_count > 0) {
    argv = calloc(arg_count, sizeof(IROperand));
    if (!argv) {
      BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
      return MTLC_NO_VALUE;
    }
    for (size_t i = 0; i < arg_count; i++) {
      const IROperand *a = value_operand(fn, args[i]);
      if (!a) {
        free(argv);
        BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
        return MTLC_NO_VALUE;
      }
      argv[i] = *a; /* shallow alias; append clones */
    }
  }
  record_type(fn->builder, return_type);
  int is_void = (return_type->kind == MTLC_TYPE_VOID);
  MtlcValue res = is_void ? MTLC_NO_VALUE : fresh_temp(fn);
  IRInstruction inst = {0};
  inst.op = IR_OP_CALL;
  inst.intrinsic = ir_intrinsic_from_name(callee);
  if (ir_intrinsic_is_atomic(inst.intrinsic)) {
    inst.address_space = MTLC_ADDRESS_SPACE_GLOBAL;
    inst.memory_order = MTLC_MEMORY_ORDER_RELAXED;
    inst.failure_memory_order = MTLC_MEMORY_ORDER_RELAXED;
    inst.memory_scope = MTLC_MEMORY_SCOPE_DEVICE;
  }
  if (!is_void) {
    const IROperand *dest = value_operand(fn, res);
    if (dest) {
      inst.dest = *dest;
    }
  }
  inst.text = (char *)callee;
  inst.arguments = argv;
  inst.argument_count = arg_count;
  inst.value_type = (MtlcType *)return_type;
  value_set_float_bits(fn, res, float_bits_of(return_type));
  emit(fn, &inst);
  free(argv); /* elements were cloned by append; free the container only */
  return res;
}

MtlcValue mtlc_address_space_alloc(MtlcFn *fn, const char *name,
                                   const MtlcType *element_type, size_t count,
                                   MtlcAddressSpace address_space) {
  if (!fn || !name || !element_type || count == 0 || count > UINT32_MAX ||
      !fn->ir->is_kernel ||
      (address_space != MTLC_ADDRESS_SPACE_WORKGROUP &&
       address_space != MTLC_ADDRESS_SPACE_PRIVATE)) {
    if (fn) BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
    return MTLC_NO_VALUE;
  }
  const MtlcType *pointer_type =
      mtlc_type_pointer_in(element_type, address_space);
  if (!pointer_type || mtlc_type_size(element_type) == 0 ||
      count > SIZE_MAX / mtlc_type_size(element_type)) {
    BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
    return MTLC_NO_VALUE;
  }
  record_type(fn->builder, element_type);
  record_type(fn->builder, pointer_type);
  MtlcValue result = push_value(fn, ir_operand_symbol(name));
  const IROperand *dest = value_operand(fn, result);
  if (!dest) return MTLC_NO_VALUE;
  IRInstruction instruction = {0};
  instruction.op = IR_OP_ADDRESS_SPACE_ALLOC;
  instruction.dest = *dest;
  instruction.rhs = ir_operand_int((long long)count);
  instruction.text = (char *)type_name(element_type);
  instruction.value_type = (MtlcType *)pointer_type;
  instruction.address_space = address_space;
  emit(fn, &instruction);
  return result;
}

MtlcValue mtlc_dynamic_workgroup_view(MtlcFn *fn, const char *name,
                                      const MtlcType *element_type) {
  if (!fn || !fn->ir || !fn->ir->is_kernel || !name || !element_type ||
      mtlc_type_size(element_type) == 0) {
    if (fn) BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
    return MTLC_NO_VALUE;
  }
  const MtlcType *pointer_type =
      mtlc_type_pointer_in(element_type, MTLC_ADDRESS_SPACE_WORKGROUP);
  if (!pointer_type) {
    BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
    return MTLC_NO_VALUE;
  }
  record_type(fn->builder, element_type);
  record_type(fn->builder, pointer_type);
  MtlcValue result = push_value(fn, ir_operand_symbol(name));
  const IROperand *dest = value_operand(fn, result);
  if (!dest) return MTLC_NO_VALUE;
  IRInstruction instruction = {0};
  instruction.op = IR_OP_ADDRESS_SPACE_ALLOC;
  instruction.dest = *dest;
  instruction.rhs = ir_operand_int(0);
  instruction.text = (char *)type_name(element_type);
  instruction.value_type = (MtlcType *)pointer_type;
  instruction.address_space = MTLC_ADDRESS_SPACE_WORKGROUP;
  emit(fn, &instruction);
  return result;
}

MtlcValue mtlc_intrinsic(MtlcFn *fn, MtlcIntrinsic intrinsic,
                         const MtlcValue *args, size_t arg_count,
                         const MtlcType *return_type) {
  const char *name = ir_intrinsic_name(intrinsic);
  int arity = ir_intrinsic_arity(intrinsic);
  if (!fn || !name || arity < 0 || (size_t)arity != arg_count ||
      !return_type ||
      (ir_intrinsic_is_atomic(intrinsic) &&
       return_type->kind != ir_intrinsic_atomic_result_kind(intrinsic)) ||
      (ir_intrinsic_is_subgroup(intrinsic) &&
       return_type->kind != ir_intrinsic_subgroup_result_kind(intrinsic))) {
    if (fn) {
      BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
    }
    return MTLC_NO_VALUE;
  }
  return mtlc_call(fn, name, args, arg_count, return_type);
}

MtlcValue mtlc_intrinsic_memory(MtlcFn *fn, MtlcIntrinsic intrinsic,
                                const MtlcValue *args, size_t arg_count,
                                const MtlcType *return_type,
                                MtlcAddressSpace address_space,
                                MtlcMemoryOrder order,
                                MtlcMemoryScope scope) {
  MtlcValue result;
  IRInstruction *instruction;
  if (!fn || !fn->ir || !fn->ir->is_kernel ||
      !ir_intrinsic_is_atomic(intrinsic) ||
      ir_intrinsic_is_compare_exchange(intrinsic) ||
      (address_space != MTLC_ADDRESS_SPACE_GENERIC &&
       address_space != MTLC_ADDRESS_SPACE_GLOBAL &&
       address_space != MTLC_ADDRESS_SPACE_WORKGROUP) ||
      order < MTLC_MEMORY_ORDER_RELAXED ||
      order > MTLC_MEMORY_ORDER_SEQ_CST ||
      (ir_intrinsic_is_atomic_load(intrinsic) &&
       order != MTLC_MEMORY_ORDER_RELAXED &&
       order != MTLC_MEMORY_ORDER_ACQUIRE &&
       order != MTLC_MEMORY_ORDER_SEQ_CST) ||
      (ir_intrinsic_is_atomic_store(intrinsic) &&
       order != MTLC_MEMORY_ORDER_RELAXED &&
       order != MTLC_MEMORY_ORDER_RELEASE &&
       order != MTLC_MEMORY_ORDER_SEQ_CST) ||
      scope < MTLC_MEMORY_SCOPE_WORK_ITEM ||
      scope > MTLC_MEMORY_SCOPE_SYSTEM ||
      (address_space == MTLC_ADDRESS_SPACE_WORKGROUP &&
       scope > MTLC_MEMORY_SCOPE_WORKGROUP)) {
    if (fn) {
      BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
    }
    return MTLC_NO_VALUE;
  }
  result = mtlc_intrinsic(fn, intrinsic, args, arg_count, return_type);
  if (fn->builder->error || fn->ir->instruction_count == 0) {
    return MTLC_NO_VALUE;
  }
  instruction = &fn->ir->instructions[fn->ir->instruction_count - 1];
  if (instruction->op != IR_OP_CALL || instruction->intrinsic != intrinsic) {
    BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
    return MTLC_NO_VALUE;
  }
  instruction->address_space = address_space;
  instruction->memory_order = order;
  instruction->failure_memory_order = MTLC_MEMORY_ORDER_RELAXED;
  instruction->memory_scope = scope;
  return result;
}

static int mtlc_compare_exchange_failure_valid(MtlcMemoryOrder success,
                                               MtlcMemoryOrder failure) {
  if (failure != MTLC_MEMORY_ORDER_RELAXED &&
      failure != MTLC_MEMORY_ORDER_ACQUIRE &&
      failure != MTLC_MEMORY_ORDER_SEQ_CST)
    return 0;
  switch (success) {
  case MTLC_MEMORY_ORDER_RELAXED:
    return failure == MTLC_MEMORY_ORDER_RELAXED;
  case MTLC_MEMORY_ORDER_ACQUIRE:
  case MTLC_MEMORY_ORDER_ACQ_REL:
    return failure == MTLC_MEMORY_ORDER_RELAXED ||
           failure == MTLC_MEMORY_ORDER_ACQUIRE;
  case MTLC_MEMORY_ORDER_RELEASE:
    return failure == MTLC_MEMORY_ORDER_RELAXED;
  case MTLC_MEMORY_ORDER_SEQ_CST:
    return 1;
  default:
    return 0;
  }
}

MtlcValue mtlc_atomic_compare_exchange(
    MtlcFn *fn, MtlcIntrinsic intrinsic, const MtlcValue args[4],
    const MtlcType *return_type, MtlcAddressSpace address_space,
    MtlcMemoryOrder success_order, MtlcMemoryOrder failure_order,
    MtlcMemoryScope scope) {
  MtlcValue result;
  IRInstruction *instruction;
  if (!fn || !fn->ir || !fn->ir->is_kernel || !args || !return_type ||
      !ir_intrinsic_is_compare_exchange(intrinsic) ||
      return_type->kind != ir_intrinsic_atomic_result_kind(intrinsic) ||
      (address_space != MTLC_ADDRESS_SPACE_GENERIC &&
       address_space != MTLC_ADDRESS_SPACE_GLOBAL &&
       address_space != MTLC_ADDRESS_SPACE_WORKGROUP) ||
      success_order < MTLC_MEMORY_ORDER_RELAXED ||
      success_order > MTLC_MEMORY_ORDER_SEQ_CST ||
      !mtlc_compare_exchange_failure_valid(success_order, failure_order) ||
      scope < MTLC_MEMORY_SCOPE_WORK_ITEM ||
      scope > MTLC_MEMORY_SCOPE_SYSTEM ||
      (address_space == MTLC_ADDRESS_SPACE_WORKGROUP &&
       scope > MTLC_MEMORY_SCOPE_WORKGROUP)) {
    if (fn) BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
    return MTLC_NO_VALUE;
  }
  result = mtlc_intrinsic(fn, intrinsic, args, 4, return_type);
  if (fn->builder->error || fn->ir->instruction_count == 0)
    return MTLC_NO_VALUE;
  instruction = &fn->ir->instructions[fn->ir->instruction_count - 1];
  if (instruction->op != IR_OP_CALL || instruction->intrinsic != intrinsic) {
    BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
    return MTLC_NO_VALUE;
  }
  instruction->address_space = address_space;
  instruction->memory_order = success_order;
  instruction->failure_memory_order = failure_order;
  instruction->memory_scope = scope;
  return result;
}

void mtlc_workgroup_barrier(MtlcFn *fn, MtlcMemoryOrder order,
                            unsigned memory_regions) {
  const unsigned supported = MTLC_MEMORY_REGION_WORKGROUP |
                             MTLC_MEMORY_REGION_GLOBAL;
  if (!fn || !fn->ir || !fn->ir->is_kernel ||
      order < MTLC_MEMORY_ORDER_ACQUIRE ||
      order > MTLC_MEMORY_ORDER_SEQ_CST || memory_regions == 0 ||
      (memory_regions & ~supported) != 0) {
    if (fn) BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
    return;
  }
  IRInstruction instruction = {0};
  instruction.op = IR_OP_BARRIER;
  instruction.memory_order = order;
  instruction.memory_scope = MTLC_MEMORY_SCOPE_WORKGROUP;
  instruction.memory_regions = memory_regions;
  emit(fn, &instruction);
}

void mtlc_async_copy_workgroup(MtlcFn *fn, MtlcValue destination,
                               MtlcValue source,
                               const MtlcType *element_type,
                               uint32_t element_count,
                               uint32_t transaction_bytes,
                               MtlcAsyncCache cache) {
  size_t element_size = element_type ? mtlc_type_size(element_type) : 0;
  int scalar_element =
      element_type &&
      (element_type->kind == MTLC_TYPE_INT8 ||
       element_type->kind == MTLC_TYPE_INT16 ||
       element_type->kind == MTLC_TYPE_INT32 ||
       element_type->kind == MTLC_TYPE_INT64 ||
       element_type->kind == MTLC_TYPE_UINT8 ||
       element_type->kind == MTLC_TYPE_UINT16 ||
       element_type->kind == MTLC_TYPE_UINT32 ||
       element_type->kind == MTLC_TYPE_UINT64 ||
       element_type->kind == MTLC_TYPE_BOOL ||
       element_type->kind == MTLC_TYPE_FLOAT32 ||
       element_type->kind == MTLC_TYPE_FLOAT64);
  if (!fn || !fn->ir || !fn->ir->is_kernel || !element_type ||
      !scalar_element || element_count == 0 || element_count > 4096 ||
      element_size == 0 ||
      element_size > 8 || (size_t)element_count > 65536u / element_size ||
      ((size_t)element_count * element_size) % 4u != 0 ||
      (transaction_bytes != 4 && transaction_bytes != 8 &&
       transaction_bytes != 16) ||
      ((size_t)element_count * element_size) % transaction_bytes != 0 ||
      (cache != MTLC_ASYNC_CACHE_ALL &&
       cache != MTLC_ASYNC_CACHE_GLOBAL) ||
      (cache == MTLC_ASYNC_CACHE_GLOBAL &&
       transaction_bytes != 16)) {
    if (fn) BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
    return;
  }
  const IROperand *destination_operand = value_operand(fn, destination);
  const IROperand *source_operand = value_operand(fn, source);
  const MtlcType *destination_type =
      mtlc_type_pointer_in(element_type, MTLC_ADDRESS_SPACE_WORKGROUP);
  const MtlcType *source_type =
      mtlc_type_pointer_in(element_type, MTLC_ADDRESS_SPACE_GLOBAL);
  if (!destination_operand || !source_operand || !destination_type ||
      !source_type) {
    BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
    return;
  }
  record_type(fn->builder, element_type);
  record_type(fn->builder, destination_type);
  record_type(fn->builder, source_type);
  IRInstruction instruction = {0};
  instruction.op = IR_OP_ASYNC_COPY;
  instruction.async_copy_element_count = element_count;
  instruction.async_copy_transaction_bytes = transaction_bytes;
  instruction.async_copy_cache = cache;
  IROperand arguments[2] = {*destination_operand, *source_operand};
  MtlcType *argument_types[2] = {(MtlcType *)destination_type,
                                 (MtlcType *)source_type};
  instruction.arguments = arguments;
  instruction.argument_types = argument_types;
  instruction.argument_count = 2;
  emit(fn, &instruction);
}

void mtlc_async_copy_commit(MtlcFn *fn) {
  if (!fn || !fn->ir || !fn->ir->is_kernel) {
    if (fn) BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
    return;
  }
  IRInstruction instruction = {0};
  instruction.op = IR_OP_ASYNC_COMMIT;
  emit(fn, &instruction);
}

void mtlc_async_copy_wait(MtlcFn *fn, uint32_t pending_groups) {
  if (!fn || !fn->ir || !fn->ir->is_kernel || pending_groups > 7) {
    if (fn) BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
    return;
  }
  IRInstruction instruction = {0};
  instruction.op = IR_OP_ASYNC_WAIT;
  instruction.async_copy_pending_groups = pending_groups;
  emit(fn, &instruction);
}

void mtlc_tensor_transfer_workgroup(
    MtlcFn *fn, const MtlcTensorTransferDesc *desc,
    const MtlcTensorTransferOperands *operands) {
  int has_view = operands && operands->prepared_view != MTLC_NO_VALUE;
  size_t count = ir_tensor_transfer_operand_count(desc, has_view);
  MtlcTypeKind storage_kind =
      desc ? ir_tensor_element_storage_kind(desc->element) : MTLC_TYPE_VOID;
  const MtlcType *element_type = mtlc_type_scalar(storage_kind);
  const MtlcType *global_type =
      mtlc_type_pointer_in(element_type, MTLC_ADDRESS_SPACE_GLOBAL);
  const MtlcType *workgroup_type =
      mtlc_type_pointer_in(element_type, MTLC_ADDRESS_SPACE_WORKGROUP);
  const MtlcType *view_type = mtlc_type_pointer_in(
      mtlc_type_scalar(MTLC_TYPE_UINT8), MTLC_ADDRESS_SPACE_GLOBAL);
  const MtlcType *coordinate_type = mtlc_type_scalar(MTLC_TYPE_INT32);
  IROperand *arguments = NULL;
  MtlcType **argument_types = NULL;
  MtlcValue handles[3 + MTLC_TENSOR_MAX_RANK];
  size_t at = 0;
  if (!fn || !fn->ir || !fn->ir->is_kernel || !operands || !count ||
      storage_kind == MTLC_TYPE_VOID || !element_type || !global_type ||
      !workgroup_type || !view_type || !coordinate_type) {
    if (fn) BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
    return;
  }
  arguments = calloc(count, sizeof(*arguments));
  argument_types = calloc(count, sizeof(*argument_types));
  if (!arguments || !argument_types) {
    free(arguments);
    free(argument_types);
    BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
    return;
  }
  handles[at++] = operands->destination;
  handles[at++] = operands->source;
  if (has_view) handles[at++] = operands->prepared_view;
  for (uint8_t dimension = 0; dimension < desc->rank; dimension++)
    handles[at++] = operands->coordinates[dimension];
  if (at != count) {
    free(arguments);
    free(argument_types);
    BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
    return;
  }
  for (size_t i = 0; i < count; i++) {
    const IROperand *operand = value_operand(fn, handles[i]);
    if (!operand) {
      free(arguments);
      free(argument_types);
      BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
      return;
    }
    arguments[i] = *operand;
  }
  if (desc->direction == MTLC_TENSOR_TRANSFER_GLOBAL_TO_WORKGROUP) {
    argument_types[0] = (MtlcType *)workgroup_type;
    argument_types[1] = (MtlcType *)global_type;
  } else {
    argument_types[0] = (MtlcType *)global_type;
    argument_types[1] = (MtlcType *)workgroup_type;
  }
  at = 2;
  if (has_view) argument_types[at++] = (MtlcType *)view_type;
  for (; at < count; at++)
    argument_types[at] = (MtlcType *)coordinate_type;

  record_type(fn->builder, element_type);
  record_type(fn->builder, global_type);
  record_type(fn->builder, workgroup_type);
  record_type(fn->builder, view_type);
  record_type(fn->builder, coordinate_type);
  IRInstruction instruction = {0};
  instruction.op = IR_OP_TENSOR_TRANSFER;
  instruction.tensor_transfer = *desc;
  instruction.tensor_transfer_has_prepared_view = has_view;
  instruction.arguments = arguments;
  instruction.argument_types = argument_types;
  instruction.argument_count = count;
  emit(fn, &instruction);
  free(arguments);
  free(argument_types);
}

static int mtlc_tensor_mma_handles(const MtlcTensorMmaDesc *desc,
                                   const MtlcTensorMmaOperands *operands,
                                   MtlcValue handles[11], size_t *out_count) {
  size_t count = 0;
  size_t expected = ir_tensor_mma_operand_count(desc);
  int needs_metadata = desc &&
                       desc->sparsity != MTLC_TENSOR_SPARSITY_DENSE;
  int needs_a_scale = desc && desc->a_scale_mode != MTLC_TENSOR_SCALE_NONE;
  int needs_b_scale = desc && desc->b_scale_mode != MTLC_TENSOR_SCALE_NONE;
  unsigned runtime_strides = ir_tensor_mma_runtime_stride_mask(desc);
  if (!operands || !out_count || expected < 4 ||
      (needs_metadata != (operands && operands->metadata != MTLC_NO_VALUE)) ||
      (needs_a_scale != (operands && operands->a_scale != MTLC_NO_VALUE)) ||
      (needs_b_scale != (operands && operands->b_scale != MTLC_NO_VALUE)) ||
      operands->runtime_stride_mask != runtime_strides) {
    return 0;
  }
  handles[count++] = operands->a;
  handles[count++] = operands->b;
  handles[count++] = operands->c;
  handles[count++] = operands->d;
  if (needs_metadata) handles[count++] = operands->metadata;
  if (needs_a_scale) handles[count++] = operands->a_scale;
  if (needs_b_scale) handles[count++] = operands->b_scale;
  if (runtime_strides & MTLC_TENSOR_RUNTIME_STRIDE_A)
    handles[count++] = operands->a_leading_dimension;
  if (runtime_strides & MTLC_TENSOR_RUNTIME_STRIDE_B)
    handles[count++] = operands->b_leading_dimension;
  if (runtime_strides & MTLC_TENSOR_RUNTIME_STRIDE_C)
    handles[count++] = operands->c_leading_dimension;
  if (runtime_strides & MTLC_TENSOR_RUNTIME_STRIDE_D)
    handles[count++] = operands->d_leading_dimension;
  if (count != expected) {
    return 0;
  }
  *out_count = count;
  return 1;
}

static int mtlc_values_same(MtlcFn *fn, MtlcValue lhs, MtlcValue rhs) {
  const IROperand *lhs_operand = value_operand(fn, lhs);
  const IROperand *rhs_operand = value_operand(fn, rhs);
  return lhs_operand && rhs_operand &&
         ir_operand_same(lhs_operand, rhs_operand);
}

void mtlc_tensor_mma_chain(MtlcFn *fn, const MtlcTensorMmaDesc *desc,
                           const MtlcTensorMmaOperands *tiles,
                           size_t tile_count) {
  size_t per_tile = ir_tensor_mma_operand_count(desc);
  if (!fn || !fn->ir || !fn->ir->is_kernel || !tiles || tile_count == 0 ||
      tile_count > UINT32_MAX || per_tile < 4 ||
      tile_count > SIZE_MAX / per_tile ||
      (tile_count > 1 &&
       (!desc || desc->accumulator_element != desc->result_element ||
        desc->c_layout != desc->d_layout ||
        ((desc->c_leading_dimension == 0) !=
         (desc->d_leading_dimension == 0)) ||
        (desc->c_leading_dimension != 0 &&
         desc->c_leading_dimension != desc->d_leading_dimension)))) {
    if (fn) BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
    return;
  }
  if (tile_count > 1) {
    MtlcValue output = tiles[0].d;
    MtlcValue output_stride = tiles[0].d_leading_dimension;
    for (size_t tile = 0; tile < tile_count; tile++) {
      if (!mtlc_values_same(fn, tiles[tile].d, output) ||
          (tile > 0 && !mtlc_values_same(fn, tiles[tile].c, output)) ||
          (desc->c_leading_dimension == 0 &&
           (!mtlc_values_same(fn, tiles[tile].d_leading_dimension,
                              output_stride) ||
            (tile > 0 &&
             !mtlc_values_same(fn, tiles[tile].c_leading_dimension,
                               output_stride))))) {
        BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
        return;
      }
    }
  }
  size_t total = per_tile * tile_count;
  IROperand *arguments = calloc(total, sizeof(*arguments));
  if (!arguments) {
    BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
    return;
  }
  for (size_t tile = 0; tile < tile_count; tile++) {
    MtlcValue handles[11];
    size_t count = 0;
    if (!mtlc_tensor_mma_handles(desc, &tiles[tile], handles, &count) ||
        count != per_tile) {
      free(arguments);
      BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
      return;
    }
    for (size_t i = 0; i < count; i++) {
      const IROperand *operand = value_operand(fn, handles[i]);
      if (!operand) {
        free(arguments);
        BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
        return;
      }
      arguments[tile * per_tile + i] = *operand;
    }
  }
  IRInstruction instruction = {0};
  instruction.op = IR_OP_TENSOR_MMA;
  instruction.arguments = arguments;
  instruction.argument_count = total;
  instruction.tensor_mma = *desc;
  instruction.tensor_mma_count = (uint32_t)tile_count;
  emit(fn, &instruction);
  free(arguments);
}

void mtlc_tensor_mma_ex(MtlcFn *fn, const MtlcTensorMmaDesc *desc,
                        const MtlcTensorMmaOperands *operands) {
  mtlc_tensor_mma_chain(fn, desc, operands, 1);
}

void mtlc_tensor_mma(MtlcFn *fn, const MtlcTensorMmaDesc *desc,
                     MtlcValue a, MtlcValue b, MtlcValue c, MtlcValue d) {
  MtlcTensorMmaOperands operands = {0};
  operands.a = a;
  operands.b = b;
  operands.c = c;
  operands.d = d;
  operands.metadata = MTLC_NO_VALUE;
  operands.a_scale = MTLC_NO_VALUE;
  operands.b_scale = MTLC_NO_VALUE;
  mtlc_tensor_mma_ex(fn, desc, &operands);
}

void mtlc_tensor_mma_strided(MtlcFn *fn, const MtlcTensorMmaDesc *desc,
                             MtlcValue a, MtlcValue b,
                             MtlcValue c, MtlcValue d,
                             MtlcValue lda, MtlcValue ldb,
                             MtlcValue ldc, MtlcValue ldd) {
  MtlcTensorMmaOperands operands = {0};
  operands.a = a;
  operands.b = b;
  operands.c = c;
  operands.d = d;
  operands.metadata = MTLC_NO_VALUE;
  operands.a_scale = MTLC_NO_VALUE;
  operands.b_scale = MTLC_NO_VALUE;
  operands.runtime_stride_mask = MTLC_TENSOR_RUNTIME_STRIDE_ALL;
  operands.a_leading_dimension = lda;
  operands.b_leading_dimension = ldb;
  operands.c_leading_dimension = ldc;
  operands.d_leading_dimension = ldd;
  mtlc_tensor_mma_ex(fn, desc, &operands);
}

void mtlc_tensor_matmul(MtlcFn *fn, const MtlcTensorMmaDesc *desc,
                        const MtlcTensorMatmulOperands *operands) {
  size_t mma_count = ir_tensor_mma_operand_count(desc);
  size_t total = ir_tensor_matmul_operand_count(desc);
  MtlcValue handles[16];
  size_t handle_count = 0;
  if (!fn || !fn->ir || !fn->ir->is_kernel || !operands ||
      !mma_count || total != mma_count + 5u ||
      !mtlc_tensor_mma_handles(desc, &operands->matrix, handles,
                               &handle_count) ||
      handle_count != mma_count) {
    if (fn) BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
    return;
  }
  handles[handle_count++] = operands->row_origin;
  handles[handle_count++] = operands->column_origin;
  handles[handle_count++] = operands->problem_m;
  handles[handle_count++] = operands->problem_n;
  handles[handle_count++] = operands->problem_k;
  if (handle_count != total) {
    BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
    return;
  }
  IROperand *arguments = calloc(total, sizeof(*arguments));
  if (!arguments) {
    BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
    return;
  }
  for (size_t i = 0; i < total; i++) {
    const IROperand *operand = value_operand(fn, handles[i]);
    if (!operand) {
      free(arguments);
      BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
      return;
    }
    arguments[i] = *operand;
  }
  IRInstruction instruction = {0};
  instruction.op = IR_OP_TENSOR_MATMUL;
  instruction.arguments = arguments;
  instruction.argument_count = total;
  instruction.tensor_mma = *desc;
  instruction.tensor_mma_count = 1;
  emit(fn, &instruction);
  free(arguments);
}

void mtlc_tensor_epilogue(
    MtlcFn *fn, const MtlcTensorEpilogueDesc *desc,
    const MtlcTensorEpilogueOperands *operands) {
  MtlcValue handles[8];
  size_t count = 0;
  size_t expected = ir_tensor_epilogue_operand_count(desc);
  int needs_bias =
      desc && desc->bias_mode != MTLC_TENSOR_BIAS_NONE;
  int needs_alpha = desc && desc->scale_output;
  int needs_beta = desc && desc->scale_bias;
  int needs_clamp =
      desc && desc->activation == MTLC_TENSOR_ACTIVATION_CLAMP;
  int needs_stride = desc && desc->leading_dimension == 0;
  int needs_bias_stride =
      desc && desc->bias_mode == MTLC_TENSOR_BIAS_MATRIX &&
      desc->bias_leading_dimension == 0;
  if (!fn || !fn->ir || !fn->ir->is_kernel || !operands || !expected ||
      (needs_bias != (operands && operands->bias != MTLC_NO_VALUE)) ||
      (needs_alpha != (operands && operands->alpha != MTLC_NO_VALUE)) ||
      (needs_beta != (operands && operands->beta != MTLC_NO_VALUE)) ||
      (needs_clamp !=
       (operands && operands->clamp_min != MTLC_NO_VALUE &&
        operands->clamp_max != MTLC_NO_VALUE)) ||
      (!needs_clamp && operands &&
       (operands->clamp_min != MTLC_NO_VALUE ||
        operands->clamp_max != MTLC_NO_VALUE)) ||
      (needs_stride !=
       (operands && operands->leading_dimension != MTLC_NO_VALUE)) ||
      (needs_bias_stride !=
       (operands && operands->bias_leading_dimension != MTLC_NO_VALUE))) {
    if (fn) BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
    return;
  }

  handles[count++] = operands->destination;
  if (needs_bias) handles[count++] = operands->bias;
  if (needs_alpha) handles[count++] = operands->alpha;
  if (needs_beta) handles[count++] = operands->beta;
  if (needs_clamp) {
    handles[count++] = operands->clamp_min;
    handles[count++] = operands->clamp_max;
  }
  if (needs_stride) handles[count++] = operands->leading_dimension;
  if (needs_bias_stride)
    handles[count++] = operands->bias_leading_dimension;
  if (count != expected) {
    BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
    return;
  }

  IROperand arguments[8];
  for (size_t i = 0; i < count; i++) {
    const IROperand *operand = value_operand(fn, handles[i]);
    if (!operand) {
      BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
      return;
    }
    arguments[i] = *operand;
  }
  IRInstruction instruction = {0};
  instruction.op = IR_OP_TENSOR_EPILOGUE;
  instruction.arguments = arguments;
  instruction.argument_count = count;
  instruction.tensor_epilogue = *desc;
  emit(fn, &instruction);
}

void mtlc_gpu_launch(MtlcFn *fn, MtlcValue kernel_handle, MtlcDim3 grid,
                     MtlcDim3 block, MtlcValue dynamic_shared_bytes,
                     MtlcValue stream, const MtlcValue *args,
                     const MtlcType *const *arg_types, size_t arg_count) {
  MtlcValue controls[IR_GPU_LAUNCH_CONTROL_ARGS] = {
      grid.x,  grid.y,  grid.z, block.x,
      block.y, block.z, dynamic_shared_bytes, stream};
  IROperand *operands = NULL;
  MtlcType **types = NULL;
  const IROperand *handle;
  size_t total = IR_GPU_LAUNCH_CONTROL_ARGS + arg_count;
  if (!fn || fn->ir->is_kernel || (arg_count > 0 && (!args || !arg_types))) {
    if (fn) {
      BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
    }
    return;
  }
  handle = value_operand(fn, kernel_handle);
  if (!handle) {
    BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
    return;
  }
  operands = calloc(total, sizeof(*operands));
  types = calloc(total, sizeof(*types));
  if (!operands || !types) {
    free(operands);
    free(types);
    BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
    return;
  }
  for (size_t i = 0; i < IR_GPU_LAUNCH_CONTROL_ARGS; i++) {
    const IROperand *value = value_operand(fn, controls[i]);
    if (!value) {
      free(operands);
      free(types);
      BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
      return;
    }
    operands[i] = *value;
  }
  for (size_t i = 0; i < arg_count; i++) {
    const IROperand *value = value_operand(fn, args[i]);
    if (!value || !kernel_parameter_type(arg_types[i])) {
      free(operands);
      free(types);
      BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
      return;
    }
    operands[IR_GPU_LAUNCH_CONTROL_ARGS + i] = *value;
    types[IR_GPU_LAUNCH_CONTROL_ARGS + i] = (MtlcType *)arg_types[i];
    record_type(fn->builder, arg_types[i]);
  }

  IRInstruction instruction = {0};
  instruction.op = IR_OP_GPU_LAUNCH;
  instruction.lhs = *handle;
  instruction.arguments = operands;
  instruction.argument_types = types;
  instruction.argument_count = total;
  emit(fn, &instruction);
  free(operands);
  free(types);
}

/* Real address of a function symbol (defined here or a declared extern):
 * IR_OP_ADDRESS_OF on a function name lowers to a RIP-relative lea with a
 * relocation, so the value is callable by the OS/CRT and comparable. */
MtlcValue mtlc_function_address(MtlcFn *fn, const char *name) {
  if (!fn || !name) {
    if (fn) {
      BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
    }
    return MTLC_NO_VALUE;
  }
  MtlcValue res = fresh_temp(fn);
  const IROperand *dest = value_operand(fn, res);
  if (!dest) {
    return MTLC_NO_VALUE;
  }
  IRInstruction inst = {0};
  inst.op = IR_OP_ADDRESS_OF;
  inst.dest = *dest;
  inst.lhs = ir_operand_symbol(name);
  if (inst.lhs.kind != IR_OPERAND_SYMBOL) {
    BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
    return MTLC_NO_VALUE;
  }
  emit(fn, &inst);
  ir_operand_destroy(&inst.lhs);
  return res;
}

/* Call through a function-pointer VALUE (e.g. one produced by
 * mtlc_function_address or loaded from memory). Arguments follow the same
 * ABI classification as direct calls; without a typed fn-pointer symbol the
 * backend classifies every argument as integer/pointer. */
MtlcValue mtlc_call_indirect(MtlcFn *fn, MtlcValue callee,
                             const MtlcValue *args, size_t arg_count,
                             const MtlcType *return_type) {
  if (!fn || !return_type) {
    if (fn) {
      BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
    }
    return MTLC_NO_VALUE;
  }
  const IROperand *code = value_operand(fn, callee);
  if (!code) {
    BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
    return MTLC_NO_VALUE;
  }
  IROperand code_copy = *code;
  IROperand *argv = NULL;
  if (arg_count > 0) {
    argv = calloc(arg_count, sizeof(IROperand));
    if (!argv) {
      BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
      return MTLC_NO_VALUE;
    }
    for (size_t i = 0; i < arg_count; i++) {
      const IROperand *a = value_operand(fn, args[i]);
      if (!a) {
        free(argv);
        BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
        return MTLC_NO_VALUE;
      }
      argv[i] = *a; /* shallow alias; append clones */
    }
  }
  record_type(fn->builder, return_type);
  int is_void = (return_type->kind == MTLC_TYPE_VOID);
  MtlcValue res = is_void ? MTLC_NO_VALUE : fresh_temp(fn);
  IRInstruction inst = {0};
  inst.op = IR_OP_CALL_INDIRECT;
  if (!is_void) {
    const IROperand *dest = value_operand(fn, res);
    if (dest) {
      inst.dest = *dest;
    }
  }
  inst.lhs = code_copy;
  inst.arguments = argv;
  inst.argument_count = arg_count;
  inst.value_type = (MtlcType *)return_type;
  value_set_float_bits(fn, res, float_bits_of(return_type));
  emit(fn, &inst);
  free(argv);
  return res;
}

MtlcValue mtlc_cast(MtlcFn *fn, MtlcValue value, const MtlcType *type) {
  if (!fn || !type) {
    if (fn) {
      BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
    }
    return MTLC_NO_VALUE;
  }
  const IROperand *v = value_operand(fn, value);
  if (!v) {
    BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
    return MTLC_NO_VALUE;
  }
  record_type(fn->builder, type);
  IROperand vc = *v;
  MtlcValue res = fresh_temp(fn);
  const IROperand *dest = value_operand(fn, res);
  if (!dest) {
    return MTLC_NO_VALUE;
  }
  IRInstruction inst = {0};
  inst.op = IR_OP_CAST;
  inst.dest = *dest;
  inst.lhs = vc;
  inst.text = (char *)type_name(type);
  inst.value_type = (MtlcType *)type;
  /* is_float/float_bits on a CAST describe the SOURCE operand (ir_lowering's
   * contract; codegen picks cvttss2si vs cvttsd2si from it). The TARGET is
   * resolved from inst.text. Setting them from the target here made the
   * emitter read a float32 source as already-64-bit — or, for a float->int
   * cast, as an integer, handing back the raw IEEE bit pattern. */
  if (vc.float_bits == 32 || vc.float_bits == 64) {
    inst.is_float = 1;
    inst.float_bits = vc.float_bits;
  }
  value_set_float_bits(fn, res, float_bits_of(type));
  emit(fn, &inst);
  return res;
}

MtlcValue mtlc_address_of(MtlcFn *fn, MtlcValue storage,
                         const MtlcType *pointer_type) {
  if (!fn || !pointer_type) {
    if (fn) {
      BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
    }
    return MTLC_NO_VALUE;
  }
  const IROperand *s = value_operand(fn, storage);
  if (!s) {
    BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
    return MTLC_NO_VALUE;
  }
  record_type(fn->builder, pointer_type);
  IROperand sc = *s;
  MtlcValue res = fresh_temp(fn);
  const IROperand *dest = value_operand(fn, res);
  if (!dest) {
    return MTLC_NO_VALUE;
  }
  IRInstruction inst = {0};
  inst.op = IR_OP_ADDRESS_OF;
  inst.dest = *dest;
  inst.lhs = sc;
  inst.value_type = (MtlcType *)pointer_type;
  emit(fn, &inst);
  return res;
}

/* Apply the load/store scalar flags the code generators key on: the element's
 * byte size travels in `rhs`, floats set is_float+float_bits, and unsigned
 * integer elements set is_unsigned (so a 32-bit load zero-extends). Mirrors
 * ir_lowering's shape exactly. */
static void apply_mem_flags(IRInstruction *inst, const MtlcType *elem) {
  inst->rhs = ir_operand_int((long long)mtlc_type_size(elem));
  if (mtlc_type_is_float(elem)) {
    inst->is_float = 1;
    inst->float_bits = (int)(elem->size * 8);
  } else if (mtlc_type_is_unsigned(elem)) {
    inst->is_unsigned = 1;
  }
}

MtlcValue mtlc_load(MtlcFn *fn, MtlcValue address, const MtlcType *elem_type) {
  if (!fn || !elem_type) {
    if (fn) {
      BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
    }
    return MTLC_NO_VALUE;
  }
  const IROperand *a = value_operand(fn, address);
  if (!a) {
    BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
    return MTLC_NO_VALUE;
  }
  record_type(fn->builder, elem_type);
  IROperand ac = *a;
  MtlcValue res = fresh_temp(fn);
  const IROperand *dest = value_operand(fn, res);
  if (!dest) {
    return MTLC_NO_VALUE;
  }
  IRInstruction inst = {0};
  inst.op = IR_OP_LOAD;
  inst.dest = *dest;
  inst.lhs = ac;
  apply_mem_flags(&inst, elem_type);
  inst.value_type = (MtlcType *)elem_type;
  if (inst.is_float) {
    fn->values[res].float_bits = inst.float_bits;
  }
  emit(fn, &inst);
  return res;
}

void mtlc_store(MtlcFn *fn, MtlcValue address, MtlcValue value,
               const MtlcType *elem_type) {
  if (!fn || !elem_type) {
    if (fn) {
      BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
    }
    return;
  }
  const IROperand *a = value_operand(fn, address);
  const IROperand *v = value_operand(fn, value);
  if (!a || !v) {
    BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
    return;
  }
  record_type(fn->builder, elem_type);
  IRInstruction inst = {0};
  inst.op = IR_OP_STORE;
  inst.dest = *a;
  inst.lhs = *v;
  apply_mem_flags(&inst, elem_type);
  emit(fn, &inst);
}

/* -------------------------------------------------------------------- indexing */

/* The pointee of a pointer descriptor, or NULL when `t` is not a pointer. */
static const MtlcType *pointer_element(const MtlcType *t) {
  if (!t || t->kind != MTLC_TYPE_POINTER || !t->base_type) {
    return NULL;
  }
  return t->base_type;
}

/* base + byte_offset, typed as the pointer itself so codegen keeps the address
 * space. A zero offset is folded here rather than left for the optimizer. */
static MtlcValue offset_pointer(MtlcFn *fn, MtlcValue base,
                                const MtlcType *pointer_type,
                                MtlcValue byte_offset) {
  return mtlc_binary(fn, "+", base, byte_offset, pointer_type);
}

MtlcValue mtlc_element_address(MtlcFn *fn, MtlcValue base,
                               const MtlcType *pointer_type, MtlcValue index) {
  if (!fn) {
    return MTLC_NO_VALUE;
  }
  const MtlcType *element = pointer_element(pointer_type);
  if (!element) {
    BUILDER_FAIL(fn->builder, "'%s' is not a pointer type",
                 pointer_type ? type_name(pointer_type) : "(null)");
    return MTLC_NO_VALUE;
  }
  size_t stride = element->size;
  if (stride == 0) {
    BUILDER_FAIL(fn->builder, "element type '%s' has no size",
                 type_name(element));
    return MTLC_NO_VALUE;
  }
  /* Checked here rather than left to the arithmetic below, which a stride of
   * one would skip entirely. */
  if (index == MTLC_NO_VALUE) {
    BUILDER_FAIL(fn->builder, "the index is not a value");
    return MTLC_NO_VALUE;
  }
  /* IR pointer arithmetic is in bytes; scaling the element index is exactly
   * the step a frontend should not have to open-code. */
  const MtlcType *i64 = mtlc_type_scalar(MTLC_TYPE_INT64);
  MtlcValue byte_offset =
      (stride == 1) ? index
                    : mtlc_binary(fn, "*", index,
                                  mtlc_const_int(fn, i64, (long long)stride),
                                  i64);
  return offset_pointer(fn, base, pointer_type, byte_offset);
}

MtlcValue mtlc_load_element(MtlcFn *fn, MtlcValue base,
                            const MtlcType *pointer_type, MtlcValue index) {
  MtlcValue address = mtlc_element_address(fn, base, pointer_type, index);
  if (address == MTLC_NO_VALUE) {
    return MTLC_NO_VALUE;
  }
  return mtlc_load(fn, address, pointer_element(pointer_type));
}

void mtlc_store_element(MtlcFn *fn, MtlcValue base,
                        const MtlcType *pointer_type, MtlcValue index,
                        MtlcValue value) {
  MtlcValue address = mtlc_element_address(fn, base, pointer_type, index);
  if (address == MTLC_NO_VALUE) {
    return;
  }
  mtlc_store(fn, address, value, pointer_element(pointer_type));
}

/* Resolve a pointer-to-struct plus a field index into (field type, offset). */
static const MtlcType *field_of(MtlcFn *fn, const MtlcType *struct_pointer_type,
                                size_t field_index, size_t *out_offset) {
  const MtlcType *record = pointer_element(struct_pointer_type);
  if (!record || record->kind != MTLC_TYPE_STRUCT) {
    BUILDER_FAIL(fn->builder, "'%s' is not a pointer to a struct",
                 struct_pointer_type ? type_name(struct_pointer_type)
                                     : "(null)");
    return NULL;
  }
  if (field_index >= record->field_count) {
    BUILDER_FAIL(fn->builder, "struct '%s' has %llu fields, not %llu",
                 type_name(record), (unsigned long long)record->field_count,
                 (unsigned long long)field_index + 1u);
    return NULL;
  }
  *out_offset = record->field_offsets ? record->field_offsets[field_index] : 0;
  return record->field_types[field_index];
}

MtlcValue mtlc_field_address(MtlcFn *fn, MtlcValue base,
                             const MtlcType *struct_pointer_type,
                             size_t field_index) {
  if (!fn) {
    return MTLC_NO_VALUE;
  }
  size_t offset = 0;
  const MtlcType *field = field_of(fn, struct_pointer_type, field_index, &offset);
  if (!field) {
    return MTLC_NO_VALUE;
  }
  /* The result points at the field's own type, in the struct pointer's space,
   * so the caller can load or store through it directly. */
  const MtlcType *field_pointer =
      mtlc_type_pointer_in(field, struct_pointer_type->address_space);
  if (!field_pointer) {
    BUILDER_FAIL(fn->builder, "could not form a pointer to field type '%s'",
                 type_name(field));
    return MTLC_NO_VALUE;
  }
  record_type(fn->builder, field_pointer);
  if (offset == 0) {
    /* Field 0 is at the base address; a cast retypes it without arithmetic. */
    return mtlc_cast(fn, base, field_pointer);
  }
  const MtlcType *i64 = mtlc_type_scalar(MTLC_TYPE_INT64);
  MtlcValue address =
      mtlc_binary(fn, "+", base, mtlc_const_int(fn, i64, (long long)offset),
                  field_pointer);
  return address;
}

MtlcValue mtlc_load_field(MtlcFn *fn, MtlcValue base,
                          const MtlcType *struct_pointer_type,
                          size_t field_index) {
  if (!fn) {
    return MTLC_NO_VALUE;
  }
  size_t offset = 0;
  const MtlcType *field = field_of(fn, struct_pointer_type, field_index, &offset);
  if (!field) {
    return MTLC_NO_VALUE;
  }
  MtlcValue address =
      mtlc_field_address(fn, base, struct_pointer_type, field_index);
  if (address == MTLC_NO_VALUE) {
    return MTLC_NO_VALUE;
  }
  return mtlc_load(fn, address, field);
}

void mtlc_store_field(MtlcFn *fn, MtlcValue base,
                      const MtlcType *struct_pointer_type, size_t field_index,
                      MtlcValue value) {
  if (!fn) {
    return;
  }
  size_t offset = 0;
  const MtlcType *field = field_of(fn, struct_pointer_type, field_index, &offset);
  if (!field) {
    return;
  }
  MtlcValue address =
      mtlc_field_address(fn, base, struct_pointer_type, field_index);
  if (address == MTLC_NO_VALUE) {
    return;
  }
  mtlc_store(fn, address, value, field);
}

/* --------------------------------------------------------------- control flow */

/* Allocated labels are named ".L<n>[_hint]". The counter lives on the builder,
 * so the name is unique across the whole module, not just this function. */
MtlcLabel mtlc_label_new(MtlcFn *fn, const char *hint) {
  if (!fn) {
    return MTLC_NO_LABEL;
  }
  if (fn->label_count == fn->label_capacity) {
    size_t next = fn->label_capacity ? fn->label_capacity * 2 : 8;
    char **grown_names = (char **)realloc(fn->labels, next * sizeof(char *));
    if (!grown_names) {
      BUILDER_FAIL(fn->builder, "out of memory allocating a label");
      return MTLC_NO_LABEL;
    }
    fn->labels = grown_names;
    unsigned char *grown_placed =
        (unsigned char *)realloc(fn->label_placed, next * sizeof(unsigned char));
    if (!grown_placed) {
      BUILDER_FAIL(fn->builder, "out of memory allocating a label");
      return MTLC_NO_LABEL;
    }
    fn->label_placed = grown_placed;
    fn->label_capacity = next;
  }

  char name[96];
  if (hint && hint[0]) {
    snprintf(name, sizeof(name), ".L%d_%s", fn->builder->label_counter++, hint);
  } else {
    snprintf(name, sizeof(name), ".L%d", fn->builder->label_counter++);
  }
  char *owned = mettle_strdup(name);
  if (!owned) {
    BUILDER_FAIL(fn->builder, "out of memory allocating a label");
    return MTLC_NO_LABEL;
  }
  fn->labels[fn->label_count] = owned;
  fn->label_placed[fn->label_count] = 0;
  return (MtlcLabel)fn->label_count++;
}

/* The name behind a handle, or NULL after reporting an invalid handle. */
static const char *label_name(MtlcFn *fn, MtlcLabel label, const char *api) {
  if (label < 0 || (size_t)label >= fn->label_count) {
    builder_fail_impl(fn->builder, api,
                      "label handle %d was not created by mtlc_label_new",
                      (int)label);
    return NULL;
  }
  return fn->labels[label];
}

void mtlc_label_here(MtlcFn *fn, MtlcLabel label) {
  if (!fn) {
    return;
  }
  const char *name = label_name(fn, label, __func__);
  if (!name) {
    return;
  }
  if (fn->label_placed[label]) {
    BUILDER_FAIL(fn->builder, "label '%s' is already placed in this function",
                 name);
    return;
  }
  fn->label_placed[label] = 1;
  mtlc_label(fn, name);
}

void mtlc_jump_to(MtlcFn *fn, MtlcLabel label) {
  if (!fn) {
    return;
  }
  const char *name = label_name(fn, label, __func__);
  if (name) {
    mtlc_jump(fn, name);
  }
}

void mtlc_branch_if_zero_to(MtlcFn *fn, MtlcValue cond, MtlcLabel label) {
  if (!fn) {
    return;
  }
  const char *name = label_name(fn, label, __func__);
  if (name) {
    mtlc_branch_if_zero(fn, cond, name);
  }
}

void mtlc_label(MtlcFn *fn, const char *label) {
  if (!fn || !label) {
    return;
  }
  IRInstruction inst = {0};
  inst.op = IR_OP_LABEL;
  inst.text = (char *)label;
  emit(fn, &inst);
}

void mtlc_jump(MtlcFn *fn, const char *label) {
  if (!fn || !label) {
    return;
  }
  IRInstruction inst = {0};
  inst.op = IR_OP_JUMP;
  inst.text = (char *)label;
  emit(fn, &inst);
}

void mtlc_branch_if_zero(MtlcFn *fn, MtlcValue cond, const char *label) {
  if (!fn || !label) {
    return;
  }
  const IROperand *c = value_operand(fn, cond);
  if (!c) {
    BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
    return;
  }
  IRInstruction inst = {0};
  inst.op = IR_OP_BRANCH_ZERO;
  inst.lhs = *c;
  inst.text = (char *)label;
  emit(fn, &inst);
}

void mtlc_return(MtlcFn *fn, MtlcValue value) {
  if (!fn) {
    return;
  }
  IRInstruction inst = {0};
  inst.op = IR_OP_RETURN;
  if (value != MTLC_NO_VALUE) {
    const IROperand *v = value_operand(fn, value);
    if (!v) {
      BUILDER_FAIL(fn->builder, "invalid argument or allocation failure");
      return;
    }
    inst.lhs = *v;
  }
  emit(fn, &inst);
}

/* -------------------------------------------------------------------- finish */

/* Register the canonical scalar type names so codegen can resolve every
 * parameter/return/local type by name (the frontend uses only scalars through
 * mtlc_type_scalar; composite types would be registered by their builder). */
static void register_scalar_types(IRProgram *program) {
  static const MtlcTypeKind kinds[] = {
      MTLC_TYPE_INT8,    MTLC_TYPE_INT16,   MTLC_TYPE_INT32,  MTLC_TYPE_INT64,
      MTLC_TYPE_UINT8,   MTLC_TYPE_UINT16,  MTLC_TYPE_UINT32, MTLC_TYPE_UINT64,
      MTLC_TYPE_BOOL,    MTLC_TYPE_FLOAT32, MTLC_TYPE_FLOAT64, MTLC_TYPE_STRING,
      MTLC_TYPE_VOID};
  for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
    const MtlcType *t = mtlc_type_scalar(kinds[i]);
    if (t) {
      ir_program_register_type(program, mtlc_type_kind_name(kinds[i]),
                               (MtlcType *)t);
    }
  }
}

/* Every branch must have somewhere to land. A jump to a label that was never
 * defined is the classic IR-builder slip -- a typo in a label string, or a
 * label allocated and then forgotten -- and it produces a module that fails
 * far away in codegen, or silently falls through. Catching it here names the
 * function and the label. */
static int compare_label_names(const void *a, const void *b) {
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int verify_branch_targets(MtlcBuilder *builder) {
  const char **defined = NULL;
  size_t defined_capacity = 0;
  int ok = 1;

  for (size_t f = 0; f < builder->fn_count && ok; f++) {
    MtlcFn *fn = builder->fns[f];
    if (!fn || !fn->ir) {
      continue;
    }
    IRFunction *irf = fn->ir;
    if (irf->instruction_count > defined_capacity) {
      const char **grown = (const char **)realloc(
          defined, irf->instruction_count * sizeof(*defined));
      if (!grown) {
        /* Verification is a courtesy; running out of memory here must not
         * condemn an otherwise valid module. */
        free(defined);
        return 1;
      }
      defined = grown;
      defined_capacity = irf->instruction_count;
    }

    /* Collect the defined labels once, then answer each branch by binary
     * search: a big generated function should not cost a quadratic scan. */
    size_t defined_count = 0;
    for (size_t i = 0; i < irf->instruction_count; i++) {
      const IRInstruction *insn = &irf->instructions[i];
      if (insn->op == IR_OP_LABEL && insn->text) {
        defined[defined_count++] = insn->text;
      }
    }
    qsort(defined, defined_count, sizeof(*defined), compare_label_names);

    for (size_t i = 0; i < irf->instruction_count; i++) {
      const IRInstruction *branch = &irf->instructions[i];
      if (branch->op != IR_OP_JUMP && branch->op != IR_OP_BRANCH_ZERO &&
          branch->op != IR_OP_BRANCH_EQ) {
        continue;
      }
      if (!branch->text) {
        continue;
      }
      if (!bsearch(&branch->text, defined, defined_count, sizeof(*defined),
                   compare_label_names)) {
        builder_fail_impl(builder, "mtlc_builder_finish",
                          "function '%s' branches to label '%s', which is "
                          "never defined",
                          irf->name ? irf->name : "?", branch->text);
        ok = 0;
        break;
      }
    }
  }
  free(defined);
  return ok;
}

MtlcModule *mtlc_builder_finish(MtlcBuilder *builder) {
  if (!builder) {
    return NULL;
  }
  if (!builder->error) {
    verify_branch_targets(builder);
  }
  if (builder->error) {
    mtlc_builder_destroy(builder);
    return NULL;
  }

  register_scalar_types(builder->program);
  /* every distinct type the builder saw, resolvable by its NAME (pointer
   * types like "int64*" included -- codegen resolves DECLARE_LOCAL/parameter
   * type names against this registry) */
  for (size_t i = 0; i < builder->seen_count; i++) {
    ir_program_register_type(builder->program, type_name(builder->seen_types[i]),
                             (MtlcType *)builder->seen_types[i]);
  }

  /* module symbol table: global variables */
  for (size_t i = 0; i < builder->global_count; i++) {
    GlobalDecl *g = &builder->globals[i];
    IRModuleSymbol entry;
    memset(&entry, 0, sizeof(entry));
    entry.name = g->name;
    entry.kind = IR_MODSYM_VARIABLE;
    entry.is_extern = g->is_extern;
    entry.type = (MtlcType *)g->type;
    if (!g->is_extern) {
      entry.has_initializer = 1;
      entry.init_is_float = 0;
      entry.init_bits = g->init_value;
    }
    ir_program_add_symbol(builder->program, &entry);
  }

  /* module symbol table: one entry per declared function */
  for (size_t i = 0; i < builder->decl_count; i++) {
    FnDecl *d = &builder->decls[i];
    IRModuleSymbol entry;
    memset(&entry, 0, sizeof(entry));
    entry.name = d->name;
    entry.kind = IR_MODSYM_FUNCTION;
    entry.is_extern = d->is_extern;
    entry.has_body = d->has_body;
    entry.is_kernel = d->is_kernel;
    entry.return_type = (MtlcType *)d->return_type;
    entry.param_count = d->param_count;
    if (d->param_count > 0) {
      MtlcType **params = calloc(d->param_count, sizeof(MtlcType *));
      for (size_t p = 0; p < d->param_count; p++) {
        params[p] = (MtlcType *)d->param_types[p];
      }
      entry.param_types = params; /* add_symbol copies the array */
      ir_program_add_symbol(builder->program, &entry);
      free(params);
    } else {
      ir_program_add_symbol(builder->program, &entry);
    }
    /* main(argc, argv) is signalled by a two-parameter main */
    if (strcmp(d->name, "main") == 0 && !d->is_extern) {
      builder->program->main_wants_argc_argv = (d->param_count == 2) ? 1 : 0;
    }
  }

  IRProgram *program = builder->program;
  builder->program = NULL; /* ownership transfers to the module */

  MtlcModule *module = mtlc_module_adopt_ir(program);
  if (!module) {
    ir_program_destroy(program);
    mtlc_builder_destroy(builder);
    return NULL;
  }
  mtlc_builder_destroy(builder);
  return module;
}
