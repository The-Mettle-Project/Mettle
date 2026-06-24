#include "ir_optimize_internal.h"

/* ============================================================================
 * Declarative algebraic rewrite engine.
 *
 * The point of this file: teaching Mettle a new integer algebraic identity is
 * adding ONE ROW to g_binary_identities below -- no new control flow, no new
 * operand-kind dispatch, no hand-rolled commutativity. The engine does the
 * matching, the operand cloning, the change tracking, and the IR plumbing.
 *
 * A rule matches an `dest = lhs <op> rhs` IR_OP_BINARY instruction by operator
 * text plus a pattern on each operand slot, then rewrites it in place. Slot `a`
 * is the "variable" slot for any rule whose action keeps or shifts a value
 * (A_KEEP / A_SHL read the operand that `a` matched); `commutative` rules are
 * also tried with the slots swapped, so a single row covers `x + 0` and
 * `0 + x`. Constant folding of `INT <op> INT` happens before the table runs
 * (ir_try_fold_integer_binary), so the patterns only ever face the
 * mixed/symbolic cases.
 *
 * Everything here is integer-only and operates on already-decomposed operands
 * (each operand is a temp/symbol/constant computed by an earlier instruction),
 * so discarding an operand -- `x * 0 -> 0` drops `x` -- is safe: the value was
 * materialized elsewhere and dead-code elimination reclaims it. Floats are left
 * untouched (NaN makes `x < x -> 0` and friends unsound).
 *
 * The second half of the file is ir_reassociate_constants_pass: a peephole that
 * folds a two-instruction chain `(x <op> c1) <op> c2` into `x <op> K`. It is
 * deliberately restricted to the cases that are bit-exact under two's-complement
 * wraparound at every operand width (+, -, *, and width-bounded <<), and it
 * proves the kept value `x` is unchanged between the producer and the use before
 * folding. This is where the table-driven identities above earn their keep: the
 * combined `x * K` / `x + 0` it produces is picked up by the table on the next
 * fixpoint iteration (e.g. `x * (8*1)` -> `x * 8` -> `x << 3`).
 * ==========================================================================*/

typedef enum {
  RWP_VAR,         /* matches any operand; the kept value when slot `a` */
  RWP_INT,         /* matches an INT operand whose value == pat.value */
  RWP_INT_NONZERO, /* matches any INT operand whose value != 0 */
  RWP_POW2,        /* matches an INT operand equal to 2^k for some k >= 1 */
  RWP_SAME         /* matches iff this operand structurally equals the other */
} IRRwPatKind;

typedef struct {
  IRRwPatKind kind;
  long long value; /* RWP_INT */
} IRRwPat;

typedef enum {
  RWA_KEEP_VAR, /* dest <- the operand slot `a` matched */
  RWA_CONST,    /* dest <- act.value */
  RWA_SHL_VAR   /* dest <- (slot `a`) << log2(the POW2 operand) */
} IRRwActKind;

typedef struct {
  IRRwActKind kind;
  long long value; /* RWA_CONST */
} IRRwAct;

typedef struct {
  const char *op;
  IRRwPat a;
  IRRwPat b;
  int commutative;
  IRRwAct act;
} IRBinaryIdentity;

#define P_ANY                                                                  \
  { RWP_VAR, 0 }
#define P_SAME                                                                 \
  { RWP_SAME, 0 }
#define P_INT(v)                                                               \
  { RWP_INT, (v) }
#define P_NZ                                                                   \
  { RWP_INT_NONZERO, 0 }
#define P_P2                                                                   \
  { RWP_POW2, 0 }
#define A_KEEP                                                                 \
  { RWA_KEEP_VAR, 0 }
#define A_INT(v)                                                               \
  { RWA_CONST, (v) }
#define A_SHL                                                                  \
  { RWA_SHL_VAR, 0 }

static const IRBinaryIdentity g_binary_identities[] = {
    /* x <op> x -- slot b matches "the other operand". */
    {"-", P_ANY, P_SAME, 0, A_INT(0)},
    {"^", P_ANY, P_SAME, 0, A_INT(0)},
    {"|", P_ANY, P_SAME, 0, A_KEEP},
    {"&", P_ANY, P_SAME, 0, A_KEEP},
    {"==", P_ANY, P_SAME, 0, A_INT(1)},
    {"<=", P_ANY, P_SAME, 0, A_INT(1)},
    {">=", P_ANY, P_SAME, 0, A_INT(1)},
    {"!=", P_ANY, P_SAME, 0, A_INT(0)},
    {"<", P_ANY, P_SAME, 0, A_INT(0)},
    {">", P_ANY, P_SAME, 0, A_INT(0)},

    /* additive */
    {"+", P_ANY, P_INT(0), 1, A_KEEP},
    {"-", P_ANY, P_INT(0), 0, A_KEEP},

    /* multiplicative -- POW2 (>= 2) before the *1 / *0 rows; they are disjoint
     * (1 and 0 are not POW2), so order only documents intent. */
    {"*", P_ANY, P_P2, 1, A_SHL},
    {"*", P_ANY, P_INT(0), 1, A_INT(0)},
    {"*", P_ANY, P_INT(1), 1, A_KEEP},
    {"/", P_ANY, P_INT(1), 0, A_KEEP},
    {"%", P_ANY, P_INT(1), 0, A_INT(0)},

    /* bitwise */
    {"&", P_ANY, P_INT(0), 1, A_INT(0)},
    {"&", P_ANY, P_INT(-1), 1, A_KEEP},
    {"|", P_ANY, P_INT(0), 1, A_KEEP},
    {"|", P_ANY, P_INT(-1), 1, A_INT(-1)}, /* x | all-ones = all-ones */
    {"^", P_ANY, P_INT(0), 1, A_KEEP},
    {"<<", P_ANY, P_INT(0), 0, A_KEEP},
    {">>", P_ANY, P_INT(0), 0, A_KEEP},
    {"<<", P_INT(0), P_ANY, 0, A_INT(0)}, /* 0 << x = 0 */
    {">>", P_INT(0), P_ANY, 0, A_INT(0)}, /* 0 >> x = 0 */

    /* logical (result is boolean; only the short-circuit constants fold) */
    {"&&", P_ANY, P_INT(0), 1, A_INT(0)},
    {"||", P_ANY, P_NZ, 1, A_INT(1)},
};

static int rw_pow2_shift(long long value, long long *shift) {
  if (value <= 0) {
    return 0;
  }
  unsigned long long u = (unsigned long long)value;
  if ((u & (u - 1ull)) != 0ull) {
    return 0;
  }
  long long amount = 0;
  while (u > 1ull) {
    u >>= 1u;
    amount++;
  }
  if (shift) {
    *shift = amount;
  }
  return 1;
}

static int rw_match(const IRRwPat *pat, const IROperand *operand,
                    const IROperand *other) {
  switch (pat->kind) {
  case RWP_VAR:
    return 1;
  case RWP_INT:
    return operand->kind == IR_OPERAND_INT && operand->int_value == pat->value;
  case RWP_INT_NONZERO:
    return operand->kind == IR_OPERAND_INT && operand->int_value != 0;
  case RWP_POW2: {
    long long shift = 0;
    return operand->kind == IR_OPERAND_INT &&
           rw_pow2_shift(operand->int_value, &shift) && shift >= 1;
  }
  case RWP_SAME:
    return ir_operand_equals(operand, other);
  }
  return 0;
}

/* dest <- base << shift, in place (base may alias the instruction's own
 * operands, so it is cloned before anything is destroyed). */
static int rw_to_shl(IRInstruction *instruction, const IROperand *base,
                     long long shift, int *changed) {
  IROperand cloned = ir_operand_none();
  if (!ir_operand_clone(base, &cloned)) {
    return 0;
  }
  char *op = mettle_strdup("<<");
  if (!op) {
    ir_operand_destroy(&cloned);
    return 0;
  }
  ir_operand_destroy(&instruction->lhs);
  ir_operand_destroy(&instruction->rhs);
  ir_instruction_clear_arguments(instruction);
  free(instruction->text);
  instruction->op = IR_OP_BINARY;
  instruction->lhs = cloned;
  instruction->rhs = ir_operand_int(shift);
  instruction->text = op;
  instruction->is_float = 0;
  instruction->ast_ref = NULL;
  if (changed) {
    *changed = 1;
  }
  return 1;
}

static int rw_apply(IRInstruction *instruction, const IRBinaryIdentity *rule,
                    int swapped, int *changed) {
  /* By construction slot `a` is the variable slot and slot `b` is the
   * constant/structural slot, so the matched operands are: */
  const IROperand *var = swapped ? &instruction->rhs : &instruction->lhs;
  const IROperand *bop = swapped ? &instruction->lhs : &instruction->rhs;

  switch (rule->act.kind) {
  case RWA_KEEP_VAR:
    return ir_rewrite_to_assign_operand(instruction, var, changed);
  case RWA_CONST:
    return ir_rewrite_to_assign_int(instruction, rule->act.value, changed);
  case RWA_SHL_VAR: {
    long long shift = 0;
    if (!rw_pow2_shift(bop->int_value, &shift)) {
      return 1;
    }
    return rw_to_shl(instruction, var, shift, changed);
  }
  }
  return 1;
}

/* Apply the first matching algebraic identity to one integer binary
 * instruction. Returns 0 only on allocation failure. */
int ir_rewrite_apply_binary_identities(IRInstruction *instruction,
                                       int *changed) {
  if (!instruction || instruction->op != IR_OP_BINARY ||
      instruction->is_float || !instruction->text) {
    return 1;
  }

  for (size_t k = 0; k < IR_ARRAY_COUNT(g_binary_identities); k++) {
    const IRBinaryIdentity *rule = &g_binary_identities[k];
    if (strcmp(rule->op, instruction->text) != 0) {
      continue;
    }
    if (rw_match(&rule->a, &instruction->lhs, &instruction->rhs) &&
        rw_match(&rule->b, &instruction->rhs, &instruction->lhs)) {
      return rw_apply(instruction, rule, 0, changed);
    }
    if (rule->commutative &&
        rw_match(&rule->a, &instruction->rhs, &instruction->lhs) &&
        rw_match(&rule->b, &instruction->lhs, &instruction->rhs)) {
      return rw_apply(instruction, rule, 1, changed);
    }
  }
  return 1;
}

/* ---------------------------------------------------------------------------
 * Constant reassociation: (x <op> c1) <op> c2  ->  x <op> K
 * ------------------------------------------------------------------------- */

/* If `instruction` is `x <op> c` (or `c <op> x` when const_either is set) with
 * c an INT and x a non-INT value, bind *x_out and *c_out and return 1. */
static int rw_split_var_const(const IRInstruction *instruction, const char *op,
                              int const_either, const IROperand **x_out,
                              long long *c_out) {
  if (instruction->op != IR_OP_BINARY || instruction->is_float ||
      !instruction->text || strcmp(instruction->text, op) != 0) {
    return 0;
  }
  if (instruction->rhs.kind == IR_OPERAND_INT &&
      instruction->lhs.kind != IR_OPERAND_INT) {
    *x_out = &instruction->lhs;
    *c_out = instruction->rhs.int_value;
    return 1;
  }
  if (const_either && instruction->lhs.kind == IR_OPERAND_INT &&
      instruction->rhs.kind != IR_OPERAND_INT) {
    *x_out = &instruction->rhs;
    *c_out = instruction->lhs.int_value;
    return 1;
  }
  return 0;
}

/* Nearest writer of temp `name` strictly before `before`, within the current
 * block (the backward scan stops at a label, so a found producer dominates the
 * use with no intervening control-flow join). */
static int rw_find_block_producer(const IRFunction *function, size_t before,
                                  const char *name, size_t *out_index) {
  for (size_t i = before; i > 0;) {
    i--;
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_NOP) {
      continue;
    }
    if (instruction->op == IR_OP_LABEL) {
      return 0;
    }
    if (ir_instruction_writes_temp(instruction) && instruction->dest.name &&
        strcmp(instruction->dest.name, name) == 0) {
      *out_index = i;
      return 1;
    }
  }
  return 0;
}

/* True if `x`'s value cannot change on the straight-line run (producer, use).
 * A temp can only change by being rewritten; a symbol can also change through a
 * call/store/asm that might alias it. */
static int rw_var_unchanged_between(const IRFunction *function, size_t producer,
                                    size_t use, const IROperand *x) {
  int x_is_symbol = (x->kind == IR_OPERAND_SYMBOL);
  for (size_t i = producer + 1; i < use; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_NOP) {
      continue;
    }
    if (ir_instruction_writes_destination(instruction) &&
        instruction->dest.name && x->name &&
        instruction->dest.kind == x->kind &&
        strcmp(instruction->dest.name, x->name) == 0) {
      return 0;
    }
    if (x_is_symbol &&
        (instruction->op == IR_OP_CALL ||
         instruction->op == IR_OP_CALL_INDIRECT ||
         instruction->op == IR_OP_STORE ||
         instruction->op == IR_OP_INLINE_ASM)) {
      return 0;
    }
  }
  return 1;
}

static int rw_set_binary(IRInstruction *instruction, const IROperand *x,
                         const char *op, long long k, int *changed) {
  IROperand base = ir_operand_none();
  if (!ir_operand_clone(x, &base)) {
    return 0;
  }
  char *text = mettle_strdup(op);
  if (!text) {
    ir_operand_destroy(&base);
    return 0;
  }
  ir_operand_destroy(&instruction->lhs);
  ir_operand_destroy(&instruction->rhs);
  ir_instruction_clear_arguments(instruction);
  free(instruction->text);
  instruction->op = IR_OP_BINARY;
  instruction->lhs = base;
  instruction->rhs = ir_operand_int(k);
  instruction->text = text;
  instruction->is_float = 0;
  instruction->ast_ref = NULL;
  if (changed) {
    *changed = 1;
  }
  return 1;
}

int ir_reassociate_constants_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *use = &function->instructions[i];
    if (use->op != IR_OP_BINARY || use->is_float || !use->text) {
      continue;
    }

    const char *uop = use->text;
    int is_add = (strcmp(uop, "+") == 0);
    int is_sub = (strcmp(uop, "-") == 0);
    int is_mul = (strcmp(uop, "*") == 0);
    int is_shl = (strcmp(uop, "<<") == 0);
    if (!is_add && !is_sub && !is_mul && !is_shl) {
      continue;
    }

    /* Extract (temp T, constant cu) from the use. For + and * the constant may
     * sit on either side; for - and << the temp must be the left operand. */
    const char *t_name = NULL;
    long long cu = 0;
    if (is_add || is_mul) {
      if (use->lhs.kind == IR_OPERAND_TEMP && use->lhs.name &&
          use->rhs.kind == IR_OPERAND_INT) {
        t_name = use->lhs.name;
        cu = use->rhs.int_value;
      } else if (use->rhs.kind == IR_OPERAND_TEMP && use->rhs.name &&
                 use->lhs.kind == IR_OPERAND_INT) {
        t_name = use->rhs.name;
        cu = use->lhs.int_value;
      }
    } else if (use->lhs.kind == IR_OPERAND_TEMP && use->lhs.name &&
               use->rhs.kind == IR_OPERAND_INT) {
      t_name = use->lhs.name;
      cu = use->rhs.int_value;
    }
    if (!t_name) {
      continue;
    }

    size_t producer_index = 0;
    if (!rw_find_block_producer(function, i, t_name, &producer_index)) {
      continue;
    }
    IRInstruction *producer = &function->instructions[producer_index];
    if (producer->op != IR_OP_BINARY || producer->is_float || !producer->text) {
      continue;
    }

    const IROperand *x = NULL;
    long long cp = 0;
    const char *new_op = NULL;
    long long combined = 0;

    if (is_add || is_sub) {
      int s_u = is_add ? 1 : -1;
      int s_p;
      if (strcmp(producer->text, "+") == 0 &&
          rw_split_var_const(producer, "+", 1, &x, &cp)) {
        s_p = 1;
      } else if (strcmp(producer->text, "-") == 0 &&
                 rw_split_var_const(producer, "-", 0, &x, &cp)) {
        s_p = -1;
      } else {
        continue;
      }
      unsigned long long term_p = (unsigned long long)((long long)s_p * cp);
      unsigned long long term_u = (unsigned long long)((long long)s_u * cu);
      combined = (long long)(term_p + term_u);
      new_op = "+";
    } else if (is_mul) {
      if (strcmp(producer->text, "*") != 0 ||
          !rw_split_var_const(producer, "*", 1, &x, &cp)) {
        continue;
      }
      combined =
          (long long)((unsigned long long)cp * (unsigned long long)cu);
      new_op = "*";
    } else { /* is_shl */
      if (cu < 0 || strcmp(producer->text, "<<") != 0 ||
          !rw_split_var_const(producer, "<<", 0, &x, &cp)) {
        continue;
      }
      /* x86 masks the shift count to the operand width; merging is only exact
       * when the combined count cannot reach 32 (covers 32- and 64-bit). */
      if (cp < 0 || cp + cu >= 32) {
        continue;
      }
      combined = cp + cu;
      new_op = "<<";
    }

    if (!x || !new_op || !x->name) {
      continue;
    }
    /* `x = x <op> cp` would make x's value at the use differ from the value the
     * algebra assumes (the producer's input). Reject self-referential producers. */
    if (producer->dest.name && producer->dest.kind == x->kind &&
        strcmp(producer->dest.name, x->name) == 0) {
      continue;
    }
    if (!rw_var_unchanged_between(function, producer_index, i, x)) {
      continue;
    }

    if (!rw_set_binary(use, x, new_op, combined, changed)) {
      return 0;
    }
  }

  return 1;
}
