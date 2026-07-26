#include "ir_optimize_internal.h"
#include "../../common.h" // mettle_free_string

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
 * is the "variable" slot for any rule whose action keeps, shifts, or masks a
 * value (A_KEEP / A_SHL / A_SHR / A_AND_MASK read the operand that `a`
 * matched); `commutative` rules are also tried with the slots swapped, so a
 * single row covers `x + 0` and `0 + x`. Constant folding of `INT <op> INT`
 * happens before the table runs (ir_try_fold_integer_binary), so the patterns
 * only ever face the mixed/symbolic cases.
 *
 * Most patterns match on operand SHAPE, which makes them true everywhere. One
 * does not: P_NONNEG asks the value-range analysis
 * (ir_optimize_value_range.c) whether the operand can be negative at THIS
 * instruction. That is what lets `x / 2^k -> x >> k` and `x % 2^k -> x & m`
 * live in the table at all -- they are ordinary identities gated on a proof
 * rather than on a literal -- and it is available to any future row. A caller
 * that passes no range context simply gets the shape-only table.
 *
 * Everything here is integer-only and operates on already-decomposed operands
 * (each operand is a temp/symbol/constant computed by an earlier instruction),
 * so discarding an operand -- `x * 0 -> 0` drops `x` -- is safe: the value was
 * materialized elsewhere and dead-code elimination reclaims it. Floats are left
 * untouched (NaN makes `x < x -> 0` and friends unsound).
 *
 * The second half of the file is ir_reassociate_constants_pass, and it follows
 * the same principle with a second table: g_const_chains has one row per
 * two-instruction chain `(x <op> c1) <op> c2` that collapses to `x <op> K`.
 * Every row is bit-exact under two's-complement wraparound at every operand
 * width -- additive (with subtraction normalized into a signed sum),
 * multiplicative, the three bitwise merges, and width-bounded shifts -- and the
 * shared driver proves the kept value `x` is unchanged between the producer and
 * the use before folding. This is where the table-driven identities above earn
 * their keep: the combined `x * K` / `x + 0` it produces is picked up by the
 * table on the next fixpoint iteration (e.g. `x * (8*1)` -> `x * 8` ->
 * `x << 3`).
 * ==========================================================================*/

typedef enum {
  RWP_VAR,         /* matches any operand; the kept value when slot `a` */
  RWP_INT,         /* matches an INT operand whose value == pat.value */
  RWP_INT_NONZERO, /* matches any INT operand whose value != 0 */
  RWP_POW2,        /* matches an INT operand equal to 2^k for some k >= 1 */
  RWP_SAME,        /* matches iff this operand structurally equals the other */
  /* Matches any operand the value-range analysis proves cannot be negative
   * HERE. This is the one pattern that depends on where the instruction sits,
   * and it is what lets a rule be conditional on a proof instead of on the
   * literal shape of its operands. Without a range context it never matches,
   * so a caller with no context simply gets the shape-only table. */
  RWP_NONNEG
} IRRwPatKind;

typedef struct {
  IRRwPatKind kind;
  long long value; /* RWP_INT */
} IRRwPat;

typedef enum {
  RWA_KEEP_VAR, /* dest <- the operand slot `a` matched */
  RWA_CONST,    /* dest <- act.value */
  RWA_SHL_VAR,  /* dest <- (slot `a`) << log2(the POW2 operand) */
  RWA_SHR_VAR,  /* dest <- (slot `a`) >> log2(the POW2 operand) */
  RWA_AND_VAR   /* dest <- (slot `a`) & (the POW2 operand - 1) */
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
#define P_NONNEG                                                               \
  { RWP_NONNEG, 0 }
#define A_KEEP                                                                 \
  { RWA_KEEP_VAR, 0 }
#define A_INT(v)                                                               \
  { RWA_CONST, (v) }
#define A_SHL                                                                  \
  { RWA_SHL_VAR, 0 }
#define A_SHR                                                                  \
  { RWA_SHR_VAR, 0 }
#define A_AND_MASK                                                             \
  { RWA_AND_VAR, 0 }

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
    /* Division and remainder by a power of two are a shift and a mask -- but
     * only for a dividend that cannot be negative, because signed division
     * truncates toward zero while a shift floors. These two rows are why the
     * table can consult the range analysis at all: everything above matches on
     * operand shape, and these match on a proof. */
    {"/", P_NONNEG, P_P2, 0, A_SHR},
    {"%", P_NONNEG, P_P2, 0, A_AND_MASK},

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
                    const IROperand *other, IRValueRangeCtx *ranges,
                    size_t at) {
  switch (pat->kind) {
  case RWP_VAR:
    return 1;
  case RWP_NONNEG:
    return ranges && ir_value_is_nonnegative(ranges, at, operand);
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

/* dest <- base <op> value, in place (base may alias the instruction's own
 * operands, so it is cloned before anything is destroyed). */
static int rw_to_binary(IRInstruction *instruction, const IROperand *base,
                        const char *text, long long value, int *changed) {
  IROperand cloned = ir_operand_none();
  if (!ir_operand_clone(base, &cloned)) {
    return 0;
  }
  char *op = mettle_strdup(text);
  if (!op) {
    ir_operand_destroy(&cloned);
    return 0;
  }
  ir_operand_destroy(&instruction->lhs);
  ir_operand_destroy(&instruction->rhs);
  ir_instruction_clear_arguments(instruction);
  mettle_free_string(instruction->text);
  instruction->op = IR_OP_BINARY;
  instruction->lhs = cloned;
  instruction->rhs = ir_operand_int(value);
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
  case RWA_SHL_VAR:
  case RWA_SHR_VAR: {
    long long shift = 0;
    if (!rw_pow2_shift(bop->int_value, &shift)) {
      return 1;
    }
    return rw_to_binary(instruction, var,
                        rule->act.kind == RWA_SHL_VAR ? "<<" : ">>", shift,
                        changed);
  }
  case RWA_AND_VAR:
    if (bop->int_value <= 0) {
      return 1;
    }
    return rw_to_binary(instruction, var, "&", bop->int_value - 1, changed);
  }
  return 1;
}

/* Apply the first matching algebraic identity to one integer binary
 * instruction. Returns 0 only on allocation failure. */
int ir_rewrite_apply_binary_identities(IRInstruction *instruction,
                                       IRValueRangeCtx *ranges, size_t at,
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
    if (rw_match(&rule->a, &instruction->lhs, &instruction->rhs, ranges, at) &&
        rw_match(&rule->b, &instruction->rhs, &instruction->lhs, ranges, at)) {
      return rw_apply(instruction, rule, 0, changed);
    }
    if (rule->commutative &&
        rw_match(&rule->a, &instruction->rhs, &instruction->lhs, ranges, at) &&
        rw_match(&rule->b, &instruction->lhs, &instruction->rhs, ranges, at)) {
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
  mettle_free_string(instruction->text);
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

/* How a rule merges the producer's constant with the use's. Every one of these
 * is bit-exact under two's-complement wraparound at every operand width, which
 * is what makes the merge legal without knowing the values' declared types. */
typedef enum {
  RWC_ADD, /* signed sum, with each side's sign from the rule */
  RWC_MUL,
  RWC_AND,
  RWC_OR,
  RWC_XOR,
  RWC_SHIFT /* shift counts add, subject to the width cap below */
} IRRwCombineKind;

typedef struct {
  const char *use_op;
  const char *producer_op;
  const char *result_op;
  IRRwCombineKind combine;
  int producer_const_either; /* the producer's constant may sit on either side */
  int use_const_either;
  int producer_sign; /* RWC_ADD only */
  int use_sign;      /* RWC_ADD only */
} IRConstChainRule;

/* One row per `(x <prod> c1) <use> c2` chain the optimizer knows how to
 * collapse. Adding a chain is adding a row; the driver below does the operand
 * extraction, the safety proof, and the rewrite.
 *
 * Subtraction is normalized into RWC_ADD by carrying each side's sign, so the
 * four +/- combinations are four rows rather than four code paths. */
static const IRConstChainRule g_const_chains[] = {
    {"+", "+", "+", RWC_ADD, 1, 1, +1, +1},
    {"+", "-", "+", RWC_ADD, 0, 1, -1, +1},
    {"-", "+", "+", RWC_ADD, 1, 0, +1, -1},
    {"-", "-", "+", RWC_ADD, 0, 0, -1, -1},
    {"*", "*", "*", RWC_MUL, 1, 1, 0, 0},
    /* Bitwise merges are exact at every width: the combined constant masks,
     * sets, or flips exactly the bits the two steps did. */
    {"&", "&", "&", RWC_AND, 1, 1, 0, 0},
    {"|", "|", "|", RWC_OR, 1, 1, 0, 0},
    {"^", "^", "^", RWC_XOR, 1, 1, 0, 0},
    /* Shift counts add. Both directions compose exactly -- the second shift
     * cannot recover bits the first discarded -- as long as the total stays
     * inside the operand width (see the cap in rw_combine_constants). */
    {"<<", "<<", "<<", RWC_SHIFT, 0, 0, 0, 0},
    {">>", ">>", ">>", RWC_SHIFT, 0, 0, 0, 0},
};

/* x86 masks a shift count to the operand width, so merging two shifts is only
 * exact when the total cannot reach the narrowest width the operands might
 * have (32 covers both 32- and 64-bit values). */
#define RW_SHIFT_MERGE_LIMIT 32

/* Does this instruction compute with unsigned semantics? The flag is the
 * primary signal; the baked result type is the fallback for instructions the
 * frontend typed but never flagged (same rule the constant evaluator uses). */
static int rw_binary_is_unsigned(const IRInstruction *in) {
  if (in->is_unsigned) {
    return 1;
  }
  if (!in->value_type) {
    return 0;
  }
  switch (in->value_type->kind) {
  case MTLC_TYPE_UINT8:
  case MTLC_TYPE_UINT16:
  case MTLC_TYPE_UINT32:
  case MTLC_TYPE_UINT64:
    return 1;
  default:
    return 0;
  }
}

static int rw_combine_constants(const IRConstChainRule *rule, long long cp,
                                long long cu, long long *out) {
  switch (rule->combine) {
  case RWC_ADD: {
    unsigned long long term_p = (unsigned long long)((long long)rule->producer_sign * cp);
    unsigned long long term_u = (unsigned long long)((long long)rule->use_sign * cu);
    *out = (long long)(term_p + term_u);
    return 1;
  }
  case RWC_MUL:
    *out = (long long)((unsigned long long)cp * (unsigned long long)cu);
    return 1;
  case RWC_AND:
    *out = cp & cu;
    return 1;
  case RWC_OR:
    *out = cp | cu;
    return 1;
  case RWC_XOR:
    *out = cp ^ cu;
    return 1;
  case RWC_SHIFT:
    if (cp < 0 || cu < 0 || cp + cu >= RW_SHIFT_MERGE_LIMIT) {
      return 0;
    }
    *out = cp + cu;
    return 1;
  }
  return 0;
}

/* Bind (temp T, constant c) from `dest = T <op> c` (or the mirrored form when
 * the rule allows it). */
static int rw_split_use(const IRInstruction *use, int const_either,
                        const char **t_name, long long *c_out) {
  if (use->lhs.kind == IR_OPERAND_TEMP && use->lhs.name &&
      use->rhs.kind == IR_OPERAND_INT) {
    *t_name = use->lhs.name;
    *c_out = use->rhs.int_value;
    return 1;
  }
  if (const_either && use->rhs.kind == IR_OPERAND_TEMP && use->rhs.name &&
      use->lhs.kind == IR_OPERAND_INT) {
    *t_name = use->rhs.name;
    *c_out = use->lhs.int_value;
    return 1;
  }
  return 0;
}

static int rw_try_chain_rule(IRFunction *function, size_t use_index,
                             const IRConstChainRule *rule, int *applied,
                             int *changed) {
  IRInstruction *use = &function->instructions[use_index];
  const char *t_name = NULL;
  long long cu = 0;

  *applied = 0;
  if (strcmp(use->text, rule->use_op) != 0 ||
      !rw_split_use(use, rule->use_const_either, &t_name, &cu)) {
    return 1;
  }

  size_t producer_index = 0;
  if (!rw_find_block_producer(function, use_index, t_name, &producer_index)) {
    return 1;
  }
  IRInstruction *producer = &function->instructions[producer_index];
  if (producer->op != IR_OP_BINARY || producer->is_float || !producer->text) {
    return 1;
  }

  const IROperand *x = NULL;
  long long cp = 0;
  if (!rw_split_var_const(producer, rule->producer_op,
                          rule->producer_const_either, &x, &cp)) {
    return 1;
  }

  /* A right shift means different things signed and unsigned, and merging two
   * of them is only exact when both agree: `(x >>arith a) >>logical b` keeps
   * the sign bits the first shift replicated, which `x >>logical (a+b)` does
   * not. Left shift and the bitwise merges carry no such distinction. */
  if (strcmp(rule->result_op, ">>") == 0 &&
      rw_binary_is_unsigned(producer) != rw_binary_is_unsigned(use)) {
    return 1;
  }

  long long combined = 0;
  if (!rw_combine_constants(rule, cp, cu, &combined) || !x || !x->name) {
    return 1;
  }

  /* `x = x <op> cp` would make x's value at the use differ from the value the
   * algebra assumes (the producer's input). Reject self-referential producers. */
  if (producer->dest.name && producer->dest.kind == x->kind &&
      strcmp(producer->dest.name, x->name) == 0) {
    return 1;
  }
  if (!rw_var_unchanged_between(function, producer_index, use_index, x)) {
    return 1;
  }

  *applied = 1;
  return rw_set_binary(use, x, rule->result_op, combined, changed);
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
    for (size_t k = 0; k < IR_ARRAY_COUNT(g_const_chains); k++) {
      int applied = 0;
      if (!rw_try_chain_rule(function, i, &g_const_chains[k], &applied,
                             changed)) {
        return 0;
      }
      if (applied) {
        break;
      }
    }
  }

  return 1;
}
