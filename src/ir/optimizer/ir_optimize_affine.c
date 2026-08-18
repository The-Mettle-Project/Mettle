#include "ir_optimize_internal.h"

/* ============================================================================
 * The shared affine loop model.
 *
 * Every loop recognizer asks the same questions before it asks its own: is
 * this a counted `while (iv < bound)` loop, is the body straight-line, does
 * the counter start at zero and step by one, is the bound loop-invariant, and
 * is this index an affine function of a symbol. Each recognizer used to
 * answer them privately, against the exact instruction shapes it happened to
 * be written for, which is where recognizer rot comes from: the shape
 * drifts, the private matcher silently stops matching.
 *
 * This module answers them once, against the model. A recognizer that
 * consumes IRAffineLoop matches semantics (a counted loop with these
 * properties) and keeps only its kernel-specific matching for itself.
 * ==========================================================================*/

/* True when `symbol` is written anywhere in [start, end). */
int ir_affine_symbol_written_in(const IRFunction *function, size_t start,
                                size_t end, const char *symbol) {
  if (!function || !symbol) {
    return 1;
  }
  for (size_t i = start; i < end && i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ir_instruction_writes_destination(ins) &&
        ir_operand_is_symbol_named(&ins->dest, symbol)) {
      return 1;
    }
  }
  return 0;
}

/* Model the counted loop whose header label sits at header_index. Fills the
 * structural facts every recognizer gates on; each is computed once, here,
 * so a recognizer never re-derives one against its own idea of the shape.
 * Returns 0 when the loop is not a counted `while (iv < bound)` loop at all.
 * The boolean facts (straight_line_body, starts_at_zero, bound_invariant,
 * unit_step) are reported, not required: a recognizer that tolerates an
 * interior branch reads the model and decides for itself. */
int ir_affine_model_loop(IRFunction *function, size_t header_index,
                         IRAffineLoop *out) {
  if (!function || !out) {
    return 0;
  }
  memset(out, 0, sizeof(*out));

  if (!ir_find_while_loop_bounds(function, header_index, &out->bounds)) {
    return 0;
  }

  const IRInstruction *compare =
      &function->instructions[out->bounds.compare_index];
  /* ir_find_while_loop_bounds already proved: integer BINARY `<` with a
   * symbol lhs feeding the branch. */
  if (compare->rhs.kind != IR_OPERAND_SYMBOL &&
      compare->rhs.kind != IR_OPERAND_INT) {
    return 0;
  }
  out->function = function;
  out->header_index = header_index;
  out->iv = compare->lhs.name;
  out->bound = compare->rhs;
  out->body_start = out->bounds.branch_index + 1;
  out->body_end = out->bounds.jump_index;
  out->c_unit_step = -1;
  out->c_starts_at_zero = -1;
  out->c_straight_line = -1;
  out->c_bound_invariant = -1;
  out->c_unclaimable = -1;
  return 1;
}

int ir_affine_unit_step(IRAffineLoop *loop) {
  if (!loop || !loop->function) {
    return 0;
  }
  if (loop->c_unit_step < 0) {
    loop->c_unit_step = 0;
    for (size_t i = loop->body_start; i < loop->body_end; i++) {
      if (ir_try_parse_direct_unit_increment(&loop->function->instructions[i],
                                             loop->iv)) {
        loop->c_unit_step = 1;
        loop->step = 1;
        loop->step_index = i;
        break;
      }
    }
  }
  return loop->c_unit_step;
}

int ir_affine_starts_at_zero(IRAffineLoop *loop) {
  if (!loop || !loop->function) {
    return 0;
  }
  if (loop->c_starts_at_zero < 0) {
    loop->c_starts_at_zero =
        ir_iv_zero_at_header(loop->function, loop->header_index, loop->iv) ? 1
                                                                          : 0;
  }
  return loop->c_starts_at_zero;
}

int ir_affine_straight_line_body(IRAffineLoop *loop) {
  if (!loop || !loop->function) {
    return 0;
  }
  if (loop->c_straight_line < 0) {
    loop->c_straight_line = 1;
    for (size_t i = loop->body_start; i < loop->body_end; i++) {
      IROpcode op = loop->function->instructions[i].op;
      if (op == IR_OP_LABEL || op == IR_OP_JUMP || op == IR_OP_BRANCH_ZERO ||
          op == IR_OP_BRANCH_EQ || op == IR_OP_CALL ||
          op == IR_OP_CALL_INDIRECT || op == IR_OP_PREFETCH) {
        loop->c_straight_line = 0;
        break;
      }
    }
  }
  return loop->c_straight_line;
}

int ir_affine_bound_invariant(IRAffineLoop *loop) {
  if (!loop || !loop->function) {
    return 0;
  }
  if (loop->c_bound_invariant < 0) {
    loop->c_bound_invariant =
        (loop->bound.kind == IR_OPERAND_INT ||
         !ir_affine_symbol_written_in(loop->function, loop->body_start,
                                      loop->body_end, loop->bound.name))
            ? 1
            : 0;
  }
  return loop->c_bound_invariant;
}

int ir_affine_body_unclaimable(IRAffineLoop *loop) {
  if (!loop || !loop->function) {
    return 1;
  }
  if (loop->c_unclaimable < 0) {
    loop->c_unclaimable = ir_loop_body_is_unclaimable(
                              loop->function, loop->body_start, loop->body_end)
                              ? 1
                              : 0;
  }
  return loop->c_unclaimable;
}

/* Overflow-checked composition of the decomposition arithmetic.
 *
 * These matter more than they look. The safe-mode elision consumes the
 * result to decide a bounds check is unnecessary, so a coefficient that
 * wrapped is not a missed optimization, it is a check removed on the strength
 * of a wrong number. Signed overflow is also undefined, which means the
 * compiler building THIS compiler is entitled to assume it never happens.
 * Every composition below refuses rather than wraps, and refusing costs only
 * the optimization. */
static int ir_affine_add(long long a, long long b, long long *out) {
  if ((b > 0 && a > LLONG_MAX - b) || (b < 0 && a < LLONG_MIN - b)) {
    return 0;
  }
  *out = a + b;
  return 1;
}

static int ir_affine_sub(long long a, long long b, long long *out) {
  if ((b < 0 && a > LLONG_MAX + b) || (b > 0 && a < LLONG_MIN + b)) {
    return 0;
  }
  *out = a - b;
  return 1;
}

static int ir_affine_mul(long long a, long long b, long long *out) {
  if (a == 0 || b == 0) {
    *out = 0;
    return 1;
  }
  if (a == -1) {
    if (b == LLONG_MIN) {
      return 0;
    }
    *out = -b;
    return 1;
  }
  if (b == -1) {
    if (a == LLONG_MIN) {
      return 0;
    }
    *out = -a;
    return 1;
  }
  if (a > 0 ? (b > 0 ? a > LLONG_MAX / b : b < LLONG_MIN / a)
            : (b > 0 ? a < LLONG_MIN / b : a < LLONG_MAX / b)) {
    return 0;
  }
  *out = a * b;
  return 1;
}

/* Left shift of a negative value is undefined, so this routes through the
 * multiply rather than shifting. */
static int ir_affine_shl(long long a, long long s, long long *out) {
  if (s < 0 || s > 62) {
    return 0;
  }
  return ir_affine_mul(a, 1LL << s, out);
}

/* Decompose an index operand as `coeff * name + addend`, following producer
 * chains: a bare symbol is (1*name + 0), an integer is (0*NULL + c), and
 * ASSIGN copies, +/- constants, * constants, and << constants compose. This
 * is the one answer to "is this index affine in something", shared by the
 * safety elision and any recognizer that reads indices. Depth-capped: an
 * index that takes more than a handful of steps to decompose is not one of
 * the shapes anything here optimizes. */
static int ir_affine_decompose_rec(const IRFunction *function, size_t before,
                                   const IROperand *index, int depth,
                                   const char **name_out, long long *coeff_out,
                                   long long *addend_out) {
  if (depth > 6) {
    return 0;
  }
  if (index->kind == IR_OPERAND_INT) {
    *name_out = NULL;
    *coeff_out = 0;
    *addend_out = index->int_value;
    return 1;
  }
  if (index->kind == IR_OPERAND_SYMBOL && index->name) {
    *name_out = index->name;
    *coeff_out = 1;
    *addend_out = 0;
    return 1;
  }
  if (index->kind != IR_OPERAND_TEMP || !index->name) {
    return 0;
  }
  const IRInstruction *producer =
      ir_find_temp_producer_before(function, before, index->name);
  if (!producer || producer->is_float) {
    return 0;
  }
  if (producer->op == IR_OP_ASSIGN) {
    return ir_affine_decompose_rec(function, before, &producer->lhs, depth + 1,
                                   name_out, coeff_out, addend_out);
  }
  if (producer->op != IR_OP_BINARY || !producer->text) {
    return 0;
  }

  const char *ln = NULL, *rn = NULL;
  long long lc = 0, la = 0, rc = 0, ra = 0;
  if (!ir_affine_decompose_rec(function, before, &producer->lhs, depth + 1,
                               &ln, &lc, &la)) {
    return 0;
  }
  if (!ir_affine_decompose_rec(function, before, &producer->rhs, depth + 1,
                               &rn, &rc, &ra)) {
    return 0;
  }

  if (strcmp(producer->text, "+") == 0) {
    if (ln && rn) {
      return 0; /* two symbols: not affine in one */
    }
    if (!ir_affine_add(lc, rc, coeff_out) ||
        !ir_affine_add(la, ra, addend_out)) {
      return 0;
    }
    *name_out = ln ? ln : rn;
    return 1;
  }
  if (strcmp(producer->text, "-") == 0) {
    if (rn) {
      return 0; /* subtracting a symbol flips its sign; no consumer wants it */
    }
    if (!ir_affine_sub(la, ra, addend_out)) {
      return 0;
    }
    *name_out = ln;
    *coeff_out = lc;
    return 1;
  }
  if (strcmp(producer->text, "*") == 0) {
    if (ln && rn) {
      return 0;
    }
    if (rn) { /* const * name */
      if (!ir_affine_mul(rc, la, coeff_out) ||
          !ir_affine_mul(ra, la, addend_out)) {
        return 0;
      }
      *name_out = rn;
      return 1;
    }
    if (!ir_affine_mul(lc, ra, coeff_out) ||
        !ir_affine_mul(la, ra, addend_out)) {
      return 0;
    }
    *name_out = ln;
    return 1;
  }
  if (strcmp(producer->text, "<<") == 0) {
    if (rn) {
      return 0;
    }
    if (!ir_affine_shl(lc, ra, coeff_out) ||
        !ir_affine_shl(la, ra, addend_out)) {
      return 0;
    }
    *name_out = ln;
    return 1;
  }
  return 0;
}

int ir_affine_index_decompose(const IRFunction *function, size_t before,
                              const IROperand *index, const char **name_out,
                              long long *coeff_out, long long *addend_out) {
  if (!function || !index || !name_out || !coeff_out || !addend_out) {
    return 0;
  }
  return ir_affine_decompose_rec(function, before, index, 0, name_out,
                                 coeff_out, addend_out);
}

/* ---- loop fingerprint ---------------------------------------------------
 *
 * A stable hash of a loop body's dataflow, independent of the names the
 * frontend happened to generate: symbols and temps hash as their order of
 * first appearance, and the operands of commutative operators combine
 * order-free. Two compiles of the same source produce the same fingerprint,
 * and most refactors that preserve the body's dataflow do too, so CI can
 * pair fingerprints with --explain claims across compiler versions: a loop
 * whose fingerprint held still while its claim flipped from vectorized to
 * scalar is recognizer rot, caught without a benchmark. */

#define IR_FP_MAX_NAMES 160

typedef struct {
  const char *names[IR_FP_MAX_NAMES];
  int count;
} IRFpNames;

static unsigned long long ir_fp_mix(unsigned long long h,
                                    unsigned long long v) {
  h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
  return h * 0x100000001b3ull;
}

static unsigned long long ir_fp_string(const char *s) {
  unsigned long long h = 0xcbf29ce484222325ull;
  while (s && *s) {
    h = (h ^ (unsigned char)*s++) * 0x100000001b3ull;
  }
  return h;
}

static unsigned long long ir_fp_name(IRFpNames *names, const char *name) {
  for (int i = 0; i < names->count; i++) {
    if (strcmp(names->names[i], name) == 0) {
      return (unsigned long long)i + 1;
    }
  }
  if (names->count < IR_FP_MAX_NAMES) {
    names->names[names->count] = name;
    return (unsigned long long)(++names->count);
  }
  return ir_fp_string(name); /* overflow: still deterministic */
}

static unsigned long long ir_fp_operand(IRFpNames *names,
                                        const IROperand *op) {
  switch (op->kind) {
  case IR_OPERAND_INT:
    return ir_fp_mix(2, (unsigned long long)op->int_value);
  case IR_OPERAND_SYMBOL:
    return ir_fp_mix(3, op->name ? ir_fp_name(names, op->name) : 0);
  case IR_OPERAND_TEMP:
    return ir_fp_mix(5, op->name ? ir_fp_name(names, op->name) : 0);
  default:
    return ir_fp_mix(7, (unsigned long long)op->kind);
  }
}

static int ir_fp_op_commutative(const IRInstruction *ins) {
  if (ins->op != IR_OP_BINARY || !ins->text) {
    return 0;
  }
  const char *t = ins->text;
  return (t[1] == 0 && (t[0] == '+' || t[0] == '*' || t[0] == '&' ||
                        t[0] == '|' || t[0] == '^')) ||
         (t[2] == 0 && (strcmp(t, "==") == 0 || strcmp(t, "!=") == 0));
}

unsigned long long ir_affine_loop_fingerprint(const IRFunction *function,
                                              const IRAffineLoop *loop) {
  IRFpNames names;
  names.count = 0;
  unsigned long long h = 0xcbf29ce484222325ull;
  if (!function || !loop) {
    return 0;
  }
  for (size_t i = loop->body_start;
       i < loop->body_end && i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_NOP) {
      continue;
    }
    h = ir_fp_mix(h, (unsigned long long)ins->op);
    h = ir_fp_mix(h, (unsigned long long)ins->is_float);
    if (ins->text) {
      h = ir_fp_mix(h, ir_fp_string(ins->text));
    }
    unsigned long long lh = ir_fp_operand(&names, &ins->lhs);
    unsigned long long rh = ir_fp_operand(&names, &ins->rhs);
    if (ir_fp_op_commutative(ins)) {
      h = ir_fp_mix(h, lh + rh);
      h = ir_fp_mix(h, lh ^ rh);
    } else {
      h = ir_fp_mix(h, lh);
      h = ir_fp_mix(h, rh);
    }
    h = ir_fp_mix(h, ir_fp_operand(&names, &ins->dest));
  }
  return h;
}
