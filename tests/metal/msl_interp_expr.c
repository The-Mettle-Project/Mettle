#include "msl_interp_compile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static MslExpr compile_unary(MslCompiler *c);
static MslExpr compile_ternary(MslCompiler *c);

MslExpr mslc_value_expr(const MslType *type) {
  MslExpr e;
  memset(&e, 0, sizeof e);
  e.kind = MSL_E_VALUE;
  e.type = type;
  return e;
}

static MslExpr const_expr(const MslType *type, uint64_t value) {
  MslExpr e = mslc_value_expr(type);
  e.kind = MSL_E_CONST;
  e.value = value;
  return e;
}

static int is_integer(const MslType *t) {
  return t->kind == MSL_K_SCALAR && msl_scalars[t->scalar].is_int && t->scalar != MSL_S_BOOL;
}

static int is_floating(const MslType *t) {
  return t->kind == MSL_K_SCALAR && msl_scalars[t->scalar].is_float;
}

static const MslType *promote(MslCompiler *c, const MslType *t) {
  if (t->kind == MSL_K_SCALAR && (t->scalar == MSL_S_CHAR || t->scalar == MSL_S_UCHAR || t->scalar == MSL_S_SHORT ||
                                  t->scalar == MSL_S_USHORT)) {
    return c->prog->scalar_types[MSL_S_INT];
  }
  return t;
}

static const MslType *bool_type(MslCompiler *c) {
  return c->prog->scalar_types[MSL_S_BOOL];
}

static size_t emit_const(MslCompiler *c, int line, const MslType *type, uint64_t value) {
  size_t insn = mslc_emit(c, MSL_OP_CONST, line);
  mslc_insn(c, insn)->a = (int64_t)value;
  mslc_insn(c, insn)->t1 = type;
  return insn;
}

static void emit_addr_local(MslCompiler *c, int local, int line) {
  size_t insn = mslc_emit(c, MSL_OP_ADDR_LOCAL, line);
  mslc_insn(c, insn)->a = local;
  mslc_insn(c, insn)->b = mslc_local_space(&c->fn->locals[local]);
}

void mslc_rvalue(MslCompiler *c, MslExpr *e, int line) {
  char tn[160];
  size_t insn;
  if (e->kind == MSL_E_VALUE) {
    return;
  }
  if (e->kind == MSL_E_VOID) {
    mslc_fail(c, line, "void value used in an expression");
  }
  if (e->kind == MSL_E_CONST) {
    mslc_fail(c, line, "'%s' constant used as a value", mslc_tname(e->type, tn, sizeof tn));
  }
  if (e->type->kind == MSL_K_ARRAY && e->kind == MSL_E_VAR) {
    int space = mslc_local_space(&c->fn->locals[e->local]);
    emit_addr_local(c, e->local, line);
    *e = mslc_value_expr(msl_pointer_type(c->prog, space, e->type->elem));
    return;
  }
  if (e->type->kind == MSL_K_ARGS || e->type->kind == MSL_K_ARRAY || e->type->kind == MSL_K_ATOMIC) {
    mslc_fail(c, line, "'%s' cannot be used as a value", mslc_tname(e->type, tn, sizeof tn));
  }
  if (e->kind == MSL_E_VAR) {
    insn = mslc_emit(c, MSL_OP_LOAD_LOCAL, line);
    mslc_insn(c, insn)->a = e->local;
  } else {
    insn = mslc_emit(c, MSL_OP_LOAD, line);
    mslc_insn(c, insn)->a = e->space;
  }
  mslc_insn(c, insn)->t1 = e->type;
  *e = mslc_value_expr(e->type);
}

void mslc_check_assign(MslCompiler *c, const MslType *target, const MslType *source, int line, const char *what) {
  char a[160];
  char b[160];
  if (target == source) {
    return;
  }
  if (target->kind == MSL_K_POINTER && source->kind == MSL_K_NULLPTR) {
    return;
  }
  mslc_fail(c, line, "%s of '%s' from '%s' needs an explicit conversion", what, mslc_tname(target, a, sizeof a),
            mslc_tname(source, b, sizeof b));
}

void mslc_check_writable(MslCompiler *c, MslExpr target, int line) {
  char tn[160];
  if (target.kind == MSL_E_VAR) {
    const MslLocal *local = &c->fn->locals[target.local];
    if (local->storage == MSL_VAR_ARGS) {
      mslc_fail(c, line, "write to constant memory: '%s' is read-only", local->name);
    }
    if (local->type->kind == MSL_K_ARRAY) {
      mslc_fail(c, line, "cannot assign to array '%s'", local->name);
    }
    return;
  }
  if (target.kind == MSL_E_MEM) {
    if (target.space == MSL_SP_CONSTANT) {
      mslc_fail(c, line, "write through a constant pointer");
    }
    if (target.type->kind == MSL_K_ARGS || target.type->kind == MSL_K_ARRAY || target.type->kind == MSL_K_ATOMIC) {
      mslc_fail(c, line, "cannot assign to '%s'", mslc_tname(target.type, tn, sizeof tn));
    }
    return;
  }
  mslc_fail(c, line, "left side of the assignment is not assignable");
}

void mslc_store(MslCompiler *c, MslExpr target, int line) {
  size_t insn;
  if (target.kind == MSL_E_VAR) {
    insn = mslc_emit(c, MSL_OP_STORE_LOCAL, line);
    mslc_insn(c, insn)->a = target.local;
  } else {
    insn = mslc_emit(c, MSL_OP_STORE, line);
    mslc_insn(c, insn)->a = target.space;
  }
  mslc_insn(c, insn)->t1 = target.type;
}

static const char *op_text(int op) {
  static const char *const names[] = {"+", "-", "*", "/", "%", "&", "|", "^", "<<", ">>", "==", "!=", "<", "<=", ">", ">="};
  return names[op];
}

static MslExpr emit_binary_insn(MslCompiler *c, int op, const MslType *operand, const MslType *right, const MslType *result, int line) {
  size_t insn = mslc_emit(c, MSL_OP_BINARY, line);
  mslc_insn(c, insn)->a = op;
  mslc_insn(c, insn)->t1 = operand;
  mslc_insn(c, insn)->t2 = right;
  return mslc_value_expr(result);
}

static int is_comparison(int op) {
  return op >= MSL_BIN_EQ;
}

static MslExpr binary_shift(MslCompiler *c, int op, const MslType *lt, const MslType *rt, int line) {
  char a[160];
  char b[160];
  if (!is_integer(lt) || !is_integer(rt)) {
    mslc_fail(c, line, "shift operands '%s' and '%s' must be integers", mslc_tname(lt, a, sizeof a), mslc_tname(rt, b, sizeof b));
  }
  return emit_binary_insn(c, op, promote(c, lt), rt, promote(c, lt), line);
}

static MslExpr binary_pointer(MslCompiler *c, int op, const MslType *lt, const MslType *rt, int line) {
  size_t insn;
  char tn[160];
  if (!is_integer(rt)) {
    mslc_fail(c, line, "pointer offset has type '%s', not an integer", mslc_tname(rt, tn, sizeof tn));
  }
  if (lt->elem->size == 0) {
    mslc_fail(c, line, "arithmetic on '%s' is outside the dialect", mslc_tname(lt, tn, sizeof tn));
  }
  insn = mslc_emit(c, MSL_OP_PTR_ADD, line);
  mslc_insn(c, insn)->a = (int64_t)lt->elem->size;
  mslc_insn(c, insn)->b = op == MSL_BIN_SUB ? -1 : 1;
  mslc_insn(c, insn)->t1 = rt;
  return mslc_value_expr(lt);
}

static int pointerish(const MslType *t) {
  return t->kind == MSL_K_POINTER || t->kind == MSL_K_NULLPTR;
}

static MslExpr binary_scalar(MslCompiler *c, int op, const MslType *t, int line) {
  char tn[160];
  if (t->scalar == MSL_S_BOOL) {
    if (op != MSL_BIN_EQ && op != MSL_BIN_NE) {
      mslc_fail(c, line, "operator '%s' on bool is outside the dialect", op_text(op));
    }
    return emit_binary_insn(c, op, t, t, bool_type(c), line);
  }
  if (is_floating(t) && (op == MSL_BIN_MOD || op == MSL_BIN_AND || op == MSL_BIN_OR || op == MSL_BIN_XOR)) {
    mslc_fail(c, line, "operator '%s' needs integers, not '%s'", op_text(op), mslc_tname(t, tn, sizeof tn));
  }
  t = promote(c, t);
  return emit_binary_insn(c, op, t, t, is_comparison(op) ? bool_type(c) : t, line);
}

MslExpr mslc_binary(MslCompiler *c, int op, MslExpr l, MslExpr r, int line) {
  const MslType *lt = l.type;
  const MslType *rt = r.type;
  char a[160];
  char b[160];
  if (op == MSL_BIN_SHL || op == MSL_BIN_SHR) {
    return binary_shift(c, op, lt, rt, line);
  }
  if ((op == MSL_BIN_ADD || op == MSL_BIN_SUB) && lt->kind == MSL_K_POINTER) {
    return binary_pointer(c, op, lt, rt, line);
  }
  if ((op == MSL_BIN_EQ || op == MSL_BIN_NE) && pointerish(lt) && pointerish(rt)) {
    if (lt->kind == MSL_K_POINTER && rt->kind == MSL_K_POINTER && lt != rt) {
      mslc_fail(c, line, "comparison of '%s' with '%s'", mslc_tname(lt, a, sizeof a), mslc_tname(rt, b, sizeof b));
    }
    return emit_binary_insn(c, op, c->prog->nullptr_type, c->prog->nullptr_type, bool_type(c), line);
  }
  if (lt != rt) {
    mslc_fail(c, line, "operands of '%s' have different types '%s' and '%s'", op_text(op), mslc_tname(lt, a, sizeof a),
              mslc_tname(rt, b, sizeof b));
  }
  if (lt->kind == MSL_K_SCALAR) {
    return binary_scalar(c, op, lt, line);
  }
  if (lt->kind == MSL_K_VECTOR && op <= MSL_BIN_DIV) {
    return emit_binary_insn(c, op, lt, lt, lt, line);
  }
  mslc_fail(c, line, "operator '%s' on '%s' is outside the dialect", op_text(op), mslc_tname(lt, a, sizeof a));
  return l;
}

static int binop_info(const MslToken *tok, int *op) {
  static const struct {
    const char *text;
    int prec;
    int op;
  } table[] = {
    {"||", 1, -1}, {"&&", 2, -2}, {"|", 3, MSL_BIN_OR}, {"^", 4, MSL_BIN_XOR}, {"&", 5, MSL_BIN_AND},
    {"==", 6, MSL_BIN_EQ}, {"!=", 6, MSL_BIN_NE}, {"<", 7, MSL_BIN_LT}, {"<=", 7, MSL_BIN_LE}, {">", 7, MSL_BIN_GT},
    {">=", 7, MSL_BIN_GE}, {"<<", 8, MSL_BIN_SHL}, {">>", 8, MSL_BIN_SHR}, {"+", 9, MSL_BIN_ADD}, {"-", 9, MSL_BIN_SUB},
    {"*", 10, MSL_BIN_MUL}, {"/", 10, MSL_BIN_DIV}, {"%", 10, MSL_BIN_MOD}
  };
  size_t i;
  if (tok->kind != MSL_TK_PUNCT) {
    return 0;
  }
  for (i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
    if (mslc_tok_is(tok, table[i].text)) {
      *op = table[i].op;
      return table[i].prec;
    }
  }
  return 0;
}

static void require_bool(MslCompiler *c, const MslExpr *e, int line, const char *what) {
  char tn[160];
  if (e->type != bool_type(c)) {
    mslc_fail(c, line, "%s has type '%s', not bool", what, mslc_tname(e->type, tn, sizeof tn));
  }
}

static MslExpr compile_binary(MslCompiler *c, int min_prec);

static MslExpr compile_logical(MslCompiler *c, MslExpr lhs, int is_and, int prec, int line) {
  MslExpr rhs;
  size_t skip;
  size_t end;
  mslc_rvalue(c, &lhs, line);
  require_bool(c, &lhs, line, is_and ? "operand of '&&'" : "operand of '||'");
  skip = mslc_emit(c, MSL_OP_JUMP_FALSE, line);
  if (is_and) {
    rhs = compile_binary(c, prec + 1);
    mslc_rvalue(c, &rhs, line);
    require_bool(c, &rhs, line, "operand of '&&'");
    end = mslc_emit(c, MSL_OP_JUMP, line);
    mslc_patch(c, skip, mslc_here(c));
    emit_const(c, line, bool_type(c), 0);
  } else {
    emit_const(c, line, bool_type(c), 1);
    end = mslc_emit(c, MSL_OP_JUMP, line);
    mslc_patch(c, skip, mslc_here(c));
    rhs = compile_binary(c, prec + 1);
    mslc_rvalue(c, &rhs, line);
    require_bool(c, &rhs, line, "operand of '||'");
  }
  mslc_patch(c, end, mslc_here(c));
  return mslc_value_expr(bool_type(c));
}

static MslExpr compile_binary(MslCompiler *c, int min_prec) {
  MslExpr lhs = compile_unary(c);
  for (;;) {
    int op = 0;
    int prec = binop_info(mslc_cur(c), &op);
    int line = mslc_cur(c)->line;
    MslExpr rhs;
    if (prec == 0 || prec < min_prec) {
      return lhs;
    }
    c->pos++;
    if (op < 0) {
      lhs = compile_logical(c, lhs, op == -2, prec, line);
      continue;
    }
    if (lhs.kind == MSL_E_CONST && lhs.type->kind == MSL_K_FLAGS && op == MSL_BIN_OR) {
      rhs = compile_binary(c, prec + 1);
      if (rhs.kind != MSL_E_CONST || rhs.type->kind != MSL_K_FLAGS) {
        mslc_fail(c, line, "mem_flags can only be combined with mem_flags");
      }
      lhs.value |= rhs.value;
      continue;
    }
    mslc_rvalue(c, &lhs, line);
    rhs = compile_binary(c, prec + 1);
    mslc_rvalue(c, &rhs, line);
    lhs = mslc_binary(c, op, lhs, rhs, line);
  }
}

static MslExpr compile_ternary(MslCompiler *c) {
  int line = mslc_cur(c)->line;
  MslExpr cond = compile_binary(c, 1);
  MslExpr a;
  MslExpr b;
  size_t skip;
  size_t end;
  char ta[160];
  char tb[160];
  if (!mslc_accept(c, "?")) {
    return cond;
  }
  mslc_rvalue(c, &cond, line);
  require_bool(c, &cond, line, "condition of '?:'");
  skip = mslc_emit(c, MSL_OP_JUMP_FALSE, line);
  a = mslc_expr(c);
  mslc_rvalue(c, &a, line);
  end = mslc_emit(c, MSL_OP_JUMP, line);
  mslc_patch(c, skip, mslc_here(c));
  mslc_expect(c, ":");
  b = compile_ternary(c);
  mslc_rvalue(c, &b, line);
  mslc_patch(c, end, mslc_here(c));
  if (a.type == b.type) {
    return mslc_value_expr(a.type);
  }
  if (pointerish(a.type) && pointerish(b.type) && (a.type->kind == MSL_K_NULLPTR || b.type->kind == MSL_K_NULLPTR)) {
    return mslc_value_expr(a.type->kind == MSL_K_NULLPTR ? b.type : a.type);
  }
  mslc_fail(c, line, "arms of '?:' have different types '%s' and '%s'", mslc_tname(a.type, ta, sizeof ta), mslc_tname(b.type, tb, sizeof tb));
  return a;
}

MslExpr mslc_expr(MslCompiler *c) {
  return compile_ternary(c);
}

static MslExpr emit_cast(MslCompiler *c, MslTypeSpec spec, MslExpr e, int line) {
  const MslType *to = spec.type;
  const MslType *from = e.type;
  char a[160];
  char b[160];
  size_t insn;
  if (spec.space != MSL_SP_NONE) {
    mslc_fail(c, line, "cast to a non-pointer type with an address space");
  }
  if (spec.vote) {
    if (from->kind != MSL_K_VOTE) {
      mslc_fail(c, line, "simd_vote::vote_t cast needs a simd_vote, not '%s'", mslc_tname(from, a, sizeof a));
    }
    return mslc_value_expr(to);
  }
  if (to == from) {
    return mslc_value_expr(to);
  }
  if (to->kind == MSL_K_SCALAR && from->kind == MSL_K_SCALAR) {
    insn = mslc_emit(c, MSL_OP_CAST, line);
    mslc_insn(c, insn)->t1 = from;
    mslc_insn(c, insn)->t2 = to;
    return mslc_value_expr(to);
  }
  if (to->kind == MSL_K_POINTER && from->kind == MSL_K_NULLPTR) {
    return mslc_value_expr(to);
  }
  if (to->kind == MSL_K_POINTER && from->kind == MSL_K_POINTER) {
    if (to->space != from->space) {
      mslc_fail(c, line, "cast from '%s' to '%s' changes the address space", mslc_tname(from, a, sizeof a), mslc_tname(to, b, sizeof b));
    }
    return mslc_value_expr(to);
  }
  mslc_fail(c, line, "cast from '%s' to '%s' is outside the dialect", mslc_tname(from, a, sizeof a), mslc_tname(to, b, sizeof b));
  return e;
}

static int paren_is_cast(MslCompiler *c) {
  const MslToken *tok = mslc_ahead(c, 1);
  if (!mslc_starts_type(c, 1)) {
    return 0;
  }
  if (mslc_tok_is(tok, "const") || mslc_tok_is(tok, "coherent") || mslc_tok_is(tok, "simd_vote") ||
      mslc_space_keyword(tok) != MSL_SP_NONE) {
    return 1;
  }
  return !mslc_tok_is(mslc_ahead(c, 2), "(");
}

static MslExpr unary_arith(MslCompiler *c, int op, int line) {
  MslExpr e = compile_unary(c);
  const MslType *t;
  char tn[160];
  size_t insn;
  mslc_rvalue(c, &e, line);
  t = e.type;
  if (op == MSL_UN_LNOT) {
    require_bool(c, &e, line, "operand of '!'");
  } else if (op == MSL_UN_BNOT) {
    if (!is_integer(t)) {
      mslc_fail(c, line, "'~' needs an integer, not '%s'", mslc_tname(t, tn, sizeof tn));
    }
    t = promote(c, t);
  } else if (is_integer(t) || is_floating(t)) {
    t = promote(c, t);
  } else if (!(t->kind == MSL_K_VECTOR)) {
    mslc_fail(c, line, "unary '-' on '%s' is outside the dialect", mslc_tname(t, tn, sizeof tn));
  }
  insn = mslc_emit(c, MSL_OP_UNARY, line);
  mslc_insn(c, insn)->a = op;
  mslc_insn(c, insn)->t1 = t;
  return mslc_value_expr(t);
}

static MslExpr unary_deref(MslCompiler *c, int line) {
  MslExpr e = compile_unary(c);
  MslExpr out;
  char tn[160];
  mslc_rvalue(c, &e, line);
  if (e.type->kind != MSL_K_POINTER) {
    mslc_fail(c, line, "'*' needs a pointer, not '%s'", mslc_tname(e.type, tn, sizeof tn));
  }
  if (e.type->elem->kind == MSL_K_ATOMIC) {
    mslc_fail(c, line, "atomic_uint is accessed only through atomic functions");
  }
  memset(&out, 0, sizeof out);
  out.kind = MSL_E_MEM;
  out.type = e.type->elem;
  out.space = e.type->space;
  return out;
}

static MslExpr unary_address(MslCompiler *c, int line) {
  MslExpr e = compile_unary(c);
  const MslLocal *local;
  if (e.kind != MSL_E_VAR) {
    mslc_fail(c, line, "'&' needs a variable");
  }
  local = &c->fn->locals[e.local];
  if (local->storage != MSL_VAR_THREAD || local->type->kind == MSL_K_ARRAY) {
    mslc_fail(c, line, "taking the address of '%s' is outside the dialect", local->name);
  }
  emit_addr_local(c, e.local, line);
  return mslc_value_expr(msl_pointer_type(c->prog, MSL_SP_THREAD, local->type));
}

static MslExpr compile_postfix(MslCompiler *c, MslExpr e);
static MslExpr compile_primary(MslCompiler *c);

static MslExpr compile_unary_inner(MslCompiler *c);

static MslExpr compile_unary(MslCompiler *c) {
  MslExpr e;
  mslc_enter(c, mslc_cur(c)->line);
  e = compile_unary_inner(c);
  mslc_leave(c);
  return e;
}

static MslExpr compile_unary_inner(MslCompiler *c) {
  const MslToken *tok = mslc_cur(c);
  int line = tok->line;
  if (mslc_tok_is(tok, "-")) {
    c->pos++;
    return unary_arith(c, MSL_UN_NEG, line);
  }
  if (mslc_tok_is(tok, "~")) {
    c->pos++;
    return unary_arith(c, MSL_UN_BNOT, line);
  }
  if (mslc_tok_is(tok, "!")) {
    c->pos++;
    return unary_arith(c, MSL_UN_LNOT, line);
  }
  if (mslc_tok_is(tok, "*")) {
    c->pos++;
    return unary_deref(c, line);
  }
  if (mslc_tok_is(tok, "&")) {
    c->pos++;
    return unary_address(c, line);
  }
  if (mslc_tok_is(tok, "++") || mslc_tok_is(tok, "--") || mslc_tok_is(tok, "+")) {
    mslc_fail(c, line, "prefix '%.*s' is outside the dialect", (int)tok->len, tok->text);
  }
  if (mslc_tok_is(tok, "(") && paren_is_cast(c)) {
    MslTypeSpec spec;
    MslExpr e;
    c->pos++;
    spec = mslc_parse_type(c);
    mslc_expect(c, ")");
    e = compile_unary(c);
    mslc_rvalue(c, &e, line);
    return emit_cast(c, spec, e, line);
  }
  return compile_postfix(c, compile_primary(c));
}

static MslExpr thread_elements(MslCompiler *c, MslExpr e, int line) {
  const MslToken *index;
  MslExpr out;
  size_t insn;
  mslc_expect(c, "(");
  mslc_expect(c, ")");
  mslc_expect(c, "[");
  index = mslc_cur(c);
  if (index->kind != MSL_TK_INT || index->int_value > 1) {
    mslc_fail(c, line, "thread_elements() is indexed by the constant 0 or 1");
  }
  c->pos++;
  mslc_expect(c, "]");
  if (e.kind == MSL_E_VAR) {
    emit_addr_local(c, e.local, line);
  }
  insn = mslc_emit(c, MSL_OP_LANE_ELEM, line);
  mslc_insn(c, insn)->a = (int64_t)index->int_value;
  mslc_insn(c, insn)->b = (int64_t)e.type->elem->size;
  memset(&out, 0, sizeof out);
  out.kind = MSL_E_MEM;
  out.type = e.type->elem;
  out.space = MSL_SP_THREAD;
  return out;
}

static MslExpr member_access(MslCompiler *c, MslExpr e, int line) {
  const MslToken *name = mslc_cur(c);
  char tn[160];
  size_t i;
  if (name->kind != MSL_TK_IDENT) {
    mslc_fail(c, line, "expected a member name");
  }
  c->pos++;
  if (e.type->kind == MSL_K_ARGS && (e.kind == MSL_E_VAR || e.kind == MSL_E_MEM)) {
    for (i = 0; i < e.type->field_count; i++) {
      const MslField *field = &e.type->fields[i];
      if (strlen(field->name) == name->len && memcmp(field->name, name->text, name->len) == 0) {
        MslExpr out;
        size_t insn;
        if (e.kind == MSL_E_VAR) {
          emit_addr_local(c, e.local, line);
        }
        insn = mslc_emit(c, MSL_OP_FIELD, line);
        mslc_insn(c, insn)->a = (int64_t)field->offset;
        memset(&out, 0, sizeof out);
        out.kind = MSL_E_MEM;
        out.type = field->type;
        out.space = e.kind == MSL_E_VAR ? MSL_SP_CONSTANT : e.space;
        return out;
      }
    }
    mslc_fail(c, line, "'%s' has no field '%.*s'", e.type->name, (int)name->len, name->text);
  }
  if (e.type->kind == MSL_K_VECTOR && name->len == 1) {
    const char *lanes = "xyzw";
    const char *hit = strchr(lanes, name->text[0]);
    if (hit != NULL && name->text[0] != 0 && (int)(hit - lanes) < e.type->count) {
      size_t insn;
      mslc_rvalue(c, &e, line);
      insn = mslc_emit(c, MSL_OP_EXTRACT, line);
      mslc_insn(c, insn)->a = hit - lanes;
      mslc_insn(c, insn)->t1 = e.type;
      return mslc_value_expr(e.type->elem);
    }
  }
  if (e.type->kind == MSL_K_MATRIX && (e.kind == MSL_E_VAR || e.kind == MSL_E_MEM) && name->len == 15 &&
      memcmp(name->text, "thread_elements", 15) == 0) {
    return thread_elements(c, e, line);
  }
  mslc_fail(c, line, "member '%.*s' of '%s' is outside the dialect", (int)name->len, name->text, mslc_tname(e.type, tn, sizeof tn));
  return e;
}

static MslExpr index_access(MslCompiler *c, MslExpr e, int line) {
  const MslLocal *local;
  MslExpr index;
  MslExpr out;
  size_t insn;
  char tn[160];
  if (e.kind != MSL_E_VAR || e.type->kind != MSL_K_ARRAY) {
    mslc_fail(c, line, "only local arrays can be indexed");
  }
  local = &c->fn->locals[e.local];
  emit_addr_local(c, e.local, line);
  index = mslc_expr(c);
  mslc_rvalue(c, &index, line);
  if (!is_integer(index.type)) {
    mslc_fail(c, line, "array index has type '%s', not an integer", mslc_tname(index.type, tn, sizeof tn));
  }
  mslc_expect(c, "]");
  insn = mslc_emit(c, MSL_OP_INDEX, line);
  mslc_insn(c, insn)->a = (int64_t)e.type->elem->size;
  mslc_insn(c, insn)->b = e.type->count;
  mslc_insn(c, insn)->t1 = index.type;
  memset(&out, 0, sizeof out);
  out.kind = MSL_E_MEM;
  out.type = e.type->elem;
  out.space = mslc_local_space(local);
  return out;
}

static MslExpr compile_postfix(MslCompiler *c, MslExpr e) {
  for (;;) {
    int line = mslc_cur(c)->line;
    if (mslc_accept(c, ".")) {
      e = member_access(c, e, line);
    } else if (mslc_accept(c, "[")) {
      e = index_access(c, e, line);
    } else if (mslc_at(c, "(")) {
      mslc_fail(c, line, "call of something that is not a function");
    } else {
      return e;
    }
  }
}

static const MslType *literal_type(MslCompiler *c, const MslToken *tok) {
  int candidates[4];
  int count = 0;
  int i;
  uint64_t v = tok->int_value;
  if (tok->int_unsigned && tok->int_long) {
    candidates[count++] = MSL_S_ULONG;
  } else if (tok->int_unsigned) {
    candidates[count++] = MSL_S_UINT;
    candidates[count++] = MSL_S_ULONG;
  } else if (tok->int_long) {
    candidates[count++] = MSL_S_LONG;
    if (tok->int_hex) {
      candidates[count++] = MSL_S_ULONG;
    }
  } else {
    candidates[count++] = MSL_S_INT;
    if (tok->int_hex) {
      candidates[count++] = MSL_S_UINT;
    }
    candidates[count++] = MSL_S_LONG;
    if (tok->int_hex) {
      candidates[count++] = MSL_S_ULONG;
    }
  }
  for (i = 0; i < count; i++) {
    int s = candidates[i];
    if ((s == MSL_S_INT && v <= 0x7fffffffull) || (s == MSL_S_UINT && v <= 0xffffffffull) ||
        (s == MSL_S_LONG && v <= 0x7fffffffffffffffull) || s == MSL_S_ULONG) {
      return c->prog->scalar_types[s];
    }
  }
  mslc_fail(c, tok->line, "integer literal %.*s is too large for its type", (int)tok->len, tok->text);
  return NULL;
}

static MslExpr call_user(MslCompiler *c, MslFunction *fn, int line) {
  size_t i;
  size_t insn;
  MslExpr out;
  if (fn->is_kernel) {
    mslc_fail(c, line, "kernel '%s' cannot be called", fn->name);
  }
  mslc_expect(c, "(");
  for (i = 0; i < fn->param_count; i++) {
    MslExpr arg;
    if (i > 0) {
      mslc_expect(c, ",");
    }
    if (mslc_at(c, ")")) {
      mslc_fail(c, line, "'%s' needs %u arguments", fn->name, (unsigned)fn->param_count);
    }
    arg = mslc_expr(c);
    mslc_rvalue(c, &arg, line);
    mslc_check_assign(c, fn->locals[i].type, arg.type, line, "argument");
  }
  if (!mslc_at(c, ")")) {
    mslc_fail(c, line, "'%s' takes %u arguments", fn->name, (unsigned)fn->param_count);
  }
  c->pos++;
  insn = mslc_emit(c, MSL_OP_CALL, line);
  mslc_insn(c, insn)->p = fn;
  mslc_add_callee(c, fn);
  out = mslc_value_expr(fn->ret);
  if (fn->ret->kind == MSL_K_VOID) {
    out.kind = MSL_E_VOID;
  }
  out.is_call = 1;
  return out;
}

static MslExpr construct_vector(MslCompiler *c, const MslType *vec, int line) {
  MslExpr first;
  int i;
  char tn[160];
  c->pos++;
  mslc_expect(c, "(");
  first = mslc_expr(c);
  mslc_rvalue(c, &first, line);
  if (mslc_accept(c, ")")) {
    if (first.type->kind != MSL_K_VECTOR || first.type->scalar != vec->scalar || first.type->count != vec->count) {
      mslc_fail(c, line, "%s(%s) is outside the dialect", vec->name, mslc_tname(first.type, tn, sizeof tn));
    }
    return mslc_value_expr(vec);
  }
  if (first.type != vec->elem) {
    mslc_fail(c, line, "%s component has type '%s', not '%s'", vec->name, mslc_tname(first.type, tn, sizeof tn), vec->elem->name);
  }
  for (i = 1; i < vec->count; i++) {
    MslExpr part;
    mslc_expect(c, ",");
    part = mslc_expr(c);
    mslc_rvalue(c, &part, line);
    if (part.type != vec->elem) {
      mslc_fail(c, line, "%s component has type '%s', not '%s'", vec->name, mslc_tname(part.type, tn, sizeof tn), vec->elem->name);
    }
  }
  mslc_expect(c, ")");
  return mslc_value_expr(vec);
}

static int bits_type_ok(const MslType *t) {
  if (t->kind == MSL_K_SCALAR) {
    return t->scalar != MSL_S_BOOL;
  }
  return t->kind == MSL_K_VECTOR && t->count != 3;
}

static MslExpr template_cast(MslCompiler *c, int reinterpret, int line) {
  MslTypeSpec spec;
  MslExpr e;
  char a[160];
  char b[160];
  size_t insn;
  c->pos++;
  mslc_expect(c, "<");
  spec = mslc_parse_type(c);
  mslc_expect(c, ">");
  mslc_expect(c, "(");
  e = mslc_expr(c);
  mslc_rvalue(c, &e, line);
  mslc_expect(c, ")");
  if (spec.space != MSL_SP_NONE || spec.vote) {
    mslc_fail(c, line, "malformed cast target");
  }
  if (reinterpret) {
    int to_int = spec.type == c->prog->scalar_types[MSL_S_LONG] || spec.type == c->prog->scalar_types[MSL_S_ULONG];
    int from_int = e.type == c->prog->scalar_types[MSL_S_LONG] || e.type == c->prog->scalar_types[MSL_S_ULONG];
    if (e.type->kind == MSL_K_POINTER && to_int) {
      insn = mslc_emit(c, MSL_OP_PTR_TO_INT, line);
    } else if (from_int && spec.type->kind == MSL_K_POINTER) {
      insn = mslc_emit(c, MSL_OP_INT_TO_PTR, line);
    } else {
      mslc_fail(c, line, "reinterpret_cast from '%s' to '%s' is outside the dialect", mslc_tname(e.type, a, sizeof a),
                mslc_tname(spec.type, b, sizeof b));
      return e;
    }
  } else {
    if (!bits_type_ok(spec.type) || !bits_type_ok(e.type) || spec.type->size != e.type->size) {
      mslc_fail(c, line, "as_type from '%s' to '%s' needs two non-bool types of equal size", mslc_tname(e.type, a, sizeof a),
                mslc_tname(spec.type, b, sizeof b));
    }
    if (spec.type == e.type) {
      return mslc_value_expr(spec.type);
    }
    insn = mslc_emit(c, MSL_OP_ASTYPE, line);
  }
  mslc_insn(c, insn)->t1 = e.type;
  mslc_insn(c, insn)->t2 = spec.type;
  return mslc_value_expr(spec.type);
}

static MslExpr make_filled(MslCompiler *c, int line) {
  MslTypeSpec spec;
  MslExpr value;
  const MslType *matrix;
  size_t insn;
  char tn[160];
  c->pos++;
  mslc_expect(c, "<");
  spec = mslc_parse_type(c);
  mslc_expect(c, ",");
  if (mslc_int_literal(c) != 8) {
    mslc_fail(c, line, "only 8x8 simdgroup matrices are in the dialect");
  }
  mslc_expect(c, ",");
  if (mslc_int_literal(c) != 8) {
    mslc_fail(c, line, "only 8x8 simdgroup matrices are in the dialect");
  }
  mslc_expect(c, ">");
  matrix = spec.type->kind == MSL_K_SCALAR ? msl_matrix_type(c->prog, spec.type->scalar) : NULL;
  if (matrix == NULL || spec.space != MSL_SP_NONE) {
    mslc_fail(c, line, "simdgroup matrix of '%s' is outside the dialect", mslc_tname(spec.type, tn, sizeof tn));
  }
  mslc_expect(c, "(");
  value = mslc_expr(c);
  mslc_rvalue(c, &value, line);
  mslc_expect(c, ")");
  mslc_check_assign(c, spec.type, value.type, line, "matrix fill value");
  insn = mslc_emit(c, MSL_OP_BUILTIN, line);
  mslc_insn(c, insn)->a = MSL_BI_FILL;
  mslc_insn(c, insn)->t1 = matrix;
  mslc_insn(c, insn)->t2 = spec.type;
  return mslc_value_expr(matrix);
}

static int name_after(const MslToken *tok, const char *prefix, const char *const *names, size_t count) {
  size_t plen = strlen(prefix);
  size_t i;
  if (tok->len <= plen || memcmp(tok->text, prefix, plen) != 0) {
    return -1;
  }
  for (i = 0; i < count; i++) {
    if (strlen(names[i]) == tok->len - plen && memcmp(tok->text + plen, names[i], tok->len - plen) == 0) {
      return (int)i;
    }
  }
  return -2;
}

static int enum_constant(MslCompiler *c, MslExpr *out) {
  static const char *const orders[] = {"relaxed", "acquire", "release", "acq_rel", "seq_cst"};
  static const char *const scopes[] = {"thread", "simdgroup", "threadgroup", "device"};
  const MslToken *tok = mslc_cur(c);
  int hit = name_after(tok, "memory_order_", orders, 5);
  if (hit == -2) {
    mslc_fail(c, tok->line, "'%.*s' is outside the dialect", (int)tok->len, tok->text);
  }
  if (hit >= 0) {
    c->pos++;
    *out = const_expr(c->prog->order_type, (uint64_t)hit);
    return 1;
  }
  hit = name_after(tok, "thread_scope_", scopes, 4);
  if (hit == -2) {
    mslc_fail(c, tok->line, "'%.*s' is outside the dialect", (int)tok->len, tok->text);
  }
  if (hit >= 0) {
    c->pos++;
    *out = const_expr(c->prog->scope_type, (uint64_t)hit);
    return 1;
  }
  return 0;
}

static MslExpr mem_flags(MslCompiler *c, int line) {
  static const char *const names[] = {"mem_none", "mem_device", "mem_threadgroup"};
  static const uint64_t values[] = {0, 1, 2};
  const MslToken *tok;
  size_t i;
  c->pos++;
  mslc_expect(c, "::");
  tok = mslc_cur(c);
  for (i = 0; i < 3; i++) {
    if (mslc_tok_is(tok, names[i])) {
      c->pos++;
      return const_expr(c->prog->flags_type, values[i]);
    }
  }
  mslc_fail(c, line, "mem_flags::%.*s is outside the dialect", (int)tok->len, tok->text);
  return const_expr(c->prog->flags_type, 0);
}

static MslExpr qualified_call(MslCompiler *c, int line) {
  const MslToken *ns = mslc_cur(c);
  const MslToken *fn;
  char name[96];
  c->pos++;
  mslc_expect(c, "::");
  fn = mslc_cur(c);
  if (fn->kind != MSL_TK_IDENT || fn->len > 60) {
    mslc_fail(c, line, "expected a function name after '::'");
  }
  snprintf(name, sizeof name, "%.*s::%.*s", (int)ns->len, ns->text, (int)fn->len, fn->text);
  c->pos++;
  if (!mslc_is_builtin(name)) {
    mslc_fail(c, line, "'%s' is outside the dialect", name);
  }
  return mslc_builtin_call(c, fn, name, line);
}

static MslExpr literal_primary(MslCompiler *c, const MslToken *tok) {
  if (tok->kind == MSL_TK_INT) {
    const MslType *type = literal_type(c, tok);
    c->pos++;
    emit_const(c, tok->line, type, tok->int_value);
    return mslc_value_expr(type);
  }
  c->pos++;
  emit_const(c, tok->line, c->prog->scalar_types[MSL_S_FLOAT], tok->float_bits);
  return mslc_value_expr(c->prog->scalar_types[MSL_S_FLOAT]);
}

static MslExpr call_by_name(MslCompiler *c, const MslToken *tok, int line) {
  MslFunction *fn = mslc_find_function(c, tok->text, tok->len);
  char name[96];
  if (fn != NULL) {
    c->pos++;
    return call_user(c, fn, line);
  }
  if (tok->len > 80) {
    mslc_fail(c, line, "unknown function");
  }
  snprintf(name, sizeof name, "%.*s", (int)tok->len, tok->text);
  if (!mslc_is_builtin(name)) {
    mslc_fail(c, line, "unknown function '%s'", name);
  }
  c->pos++;
  return mslc_builtin_call(c, tok, name, line);
}

static MslExpr ident_primary(MslCompiler *c, const MslToken *tok) {
  int line = tok->line;
  const MslType *vec;
  MslExpr e;
  int local;
  if (mslc_tok_is(tok, "true") || mslc_tok_is(tok, "false")) {
    c->pos++;
    emit_const(c, line, bool_type(c), mslc_tok_is(tok, "true") ? 1u : 0u);
    return mslc_value_expr(bool_type(c));
  }
  if (mslc_tok_is(tok, "nullptr")) {
    c->pos++;
    mslc_emit(c, MSL_OP_NULLPTR, line);
    return mslc_value_expr(c->prog->nullptr_type);
  }
  if (mslc_tok_is(tok, "as_type") || mslc_tok_is(tok, "reinterpret_cast")) {
    return template_cast(c, mslc_tok_is(tok, "reinterpret_cast"), line);
  }
  if (mslc_tok_is(tok, "make_filled_simdgroup_matrix")) {
    return make_filled(c, line);
  }
  if (mslc_tok_is(tok, "os_log_default")) {
    return mslc_os_log(c, line);
  }
  if (mslc_tok_is(tok, "sizeof")) {
    mslc_fail(c, line, "sizeof is only allowed inside static_assert");
  }
  if (mslc_tok_is(tok, "mem_flags")) {
    return mem_flags(c, line);
  }
  if (mslc_tok_is(tok, "precise") || mslc_tok_is(tok, "fast")) {
    return qualified_call(c, line);
  }
  if (enum_constant(c, &e)) {
    return e;
  }
  vec = msl_vector_by_name(c->prog, tok->text, tok->len);
  if (vec != NULL && mslc_tok_is(mslc_ahead(c, 1), "(")) {
    return construct_vector(c, vec, line);
  }
  if (mslc_tok_is(mslc_ahead(c, 1), "(")) {
    return call_by_name(c, tok, line);
  }
  local = mslc_find_local(c, tok->text, tok->len);
  if (local < 0) {
    mslc_fail(c, line, "unknown identifier '%.*s'", (int)tok->len, tok->text);
  }
  c->pos++;
  memset(&e, 0, sizeof e);
  e.kind = MSL_E_VAR;
  e.local = local;
  e.type = c->fn->locals[local].type;
  return e;
}

static MslExpr compile_primary(MslCompiler *c) {
  const MslToken *tok = mslc_cur(c);
  char found[64];
  if (tok->kind == MSL_TK_INT || tok->kind == MSL_TK_FLOAT) {
    return literal_primary(c, tok);
  }
  if (tok->kind == MSL_TK_STRING) {
    mslc_fail(c, tok->line, "string literal outside os_log");
  }
  if (mslc_tok_is(tok, "(")) {
    MslExpr e;
    c->pos++;
    e = mslc_expr(c);
    mslc_expect(c, ")");
    e.is_call = 0;
    return e;
  }
  if (tok->kind == MSL_TK_IDENT) {
    return ident_primary(c, tok);
  }
  snprintf(found, sizeof found, "%.*s", (int)(tok->len < 40 ? tok->len : 40), tok->text);
  mslc_fail(c, tok->line, "expected an expression but found '%s'", found);
  return mslc_value_expr(NULL);
}
