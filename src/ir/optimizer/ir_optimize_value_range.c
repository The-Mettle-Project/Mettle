#include "ir_optimize_internal.h"
#include "../../common.h" // mettle_free_string

/* ============================================================================
 * Integer value-range analysis, and the range-driven rewrites it enables.
 *
 * The point of this file: the optimizer used to learn "this value cannot be
 * negative" one loop shape at a time. `positive_loop_div2_to_shift` turned
 * `x / 2` into `x >> 1` -- but ONLY for a symbol tested by `while (x > 1)`,
 * only for the divisor 2, and only for the first division in the body.
 * `mod_even_bitcheck` turned `x % 2^k` into `x & (2^k-1)` -- but only when the
 * result was immediately compared against zero and used exactly once. Every
 * other `n / 8`, `i % 16`, or `if (x >= 0) ... x / 4` in the language paid for
 * a full signed divide (or, on x86, the four-instruction biased-shift sequence
 * the magic-number lowering emits for a signed power-of-two divisor).
 *
 * What replaces them is one question asked generically: what is the range of
 * this operand HERE? Answer it once and a whole family of rewrites falls out
 * of the same fact -- power-of-two divide and remainder, whole-operation folds
 * (`x % c` is `x` when `0 <= x < c`), redundant mask removal, decided
 * comparisons, and resolved branches. Widening what the optimizer can prove
 * means adding a case to vr_binary_range, and EVERY rewrite gets stronger at
 * once.
 *
 * The rewrites themselves live in two places, split by what they need. The
 * ones that are ordinary algebraic identities gated on a proof -- `x / 2^k`
 * into a shift, `x % 2^k` into a mask -- are ROWS in the identity table in
 * ir_optimize_rewrite.c, which reaches back here through its P_NONNEG
 * pattern. The ones that need the computed bounds themselves, and so cannot
 * be written as a pattern over operand shapes, are ir_value_range_simplify
 * below.
 *
 * The analysis is a bounded on-demand backward walk, not a dataflow fixpoint:
 * ranges are asked for at a handful of instructions per function (a divide, a
 * remainder, a mask, a comparison against a constant), so paying a capped
 * walk per question is far cheaper than maintaining a lattice over every
 * value. Both the recursion depth and the per-level instruction scan are
 * capped, which bounds the cost of a query to a constant.
 *
 * Where the facts come from:
 *   - integer literals, and the declared width/signedness of a local or
 *     parameter (a `uint16` local is [0, 65535] by construction);
 *   - the producing instruction of a temp, or the reaching definition of a
 *     symbol when it lies in the same straight-line region;
 *   - the operator itself: a comparison yields [0,1], `&` with a non-negative
 *     operand cannot exceed it, `%` by a positive constant is bounded by it,
 *     add, subtract, multiply, and shifts propagate bounds when they
 *     provably cannot overflow;
 *   - a select is bounded by the hull of its two arms, and a logical negation
 *     is `[0,1]`;
 *   - dominating guards, on BOTH sides of a branch: falling through
 *     `branch_zero %t, L` means the condition that produced `%t` held, and
 *     arriving at a label whose only entry is that branch means it did not.
 *     So `while (n > 0) { ... }`, `if (i <= 15) { ... }`, and
 *     `if (n < 0) { return; } ...` all bound their region. This is what
 *     subsumes the old loop-shaped pass -- the loop test is just one guard
 *     among all the guards a region can carry -- and the bound may itself be
 *     a value rather than a literal, so `i < n` bounds `i` by whatever bounds
 *     `n`;
 *   - monotone counters: a 64-bit signed local whose every definition is a
 *     non-negative constant or a step forward by a positive constant.
 *
 * Everything here is integer-only and signed-64 valued. `uint64` is
 * deliberately given the full range: its upper half does not fit a
 * `long long`, and every rewrite below would be wrong for a value the
 * analysis reported as negative.
 * ==========================================================================*/

#define VR_MIN LLONG_MIN
#define VR_MAX LLONG_MAX
/* Cost caps. A query costs at most VR_MAX_DEPTH levels of VR_MAX_SCAN
 * instructions each; both are generous for the straight-line regions real
 * definitions live in, and neither lets a query scale with function size. */
#define VR_MAX_DEPTH 4
#define VR_MAX_SCAN 192

static void vr_full(IRIntRange *r) {
  r->lo = VR_MIN;
  r->hi = VR_MAX;
}

static int vr_is_full(const IRIntRange *r) {
  return r->lo == VR_MIN && r->hi == VR_MAX;
}

/* Intersection, ignoring a contradictory result: an empty range means the
 * program point is unreachable, and reporting "unreachable" as a range would
 * let a rewrite claim anything. Keep the wider fact instead. */
static void vr_intersect(IRIntRange *r, const IRIntRange *other) {
  long long lo = other->lo > r->lo ? other->lo : r->lo;
  long long hi = other->hi < r->hi ? other->hi : r->hi;
  if (lo <= hi) {
    r->lo = lo;
    r->hi = hi;
  }
}

/* A value stored into a slot of `limit`'s width wraps to that width. When the
 * computed range already fits, the wrap is a no-op and the sharper range
 * survives; otherwise all we know is the slot's own range. */
static void vr_narrow_to(IRIntRange *r, const IRIntRange *limit) {
  if (vr_is_full(limit)) {
    return;
  }
  if (r->lo >= limit->lo && r->hi <= limit->hi) {
    return;
  }
  *r = *limit;
}

/* Range of a `bits`-wide integer type. uint64 is left unbounded: its upper
 * half is negative as a long long, which every consumer here would misread. */
static void vr_of_int_type(int bits, int is_unsigned, IRIntRange *r) {
  if (bits <= 0 || bits > 64 || (bits == 64 && is_unsigned)) {
    vr_full(r);
    return;
  }
  if (bits == 64) {
    vr_full(r);
    return;
  }
  if (is_unsigned) {
    r->lo = 0;
    r->hi = (long long)((1ull << bits) - 1ull);
  } else {
    r->hi = (long long)((1ull << (bits - 1)) - 1ull);
    r->lo = -r->hi - 1;
  }
}

int ir_int_type_name_info(const char *name, int *bits_out,
                          int *is_unsigned_out) {
  int bits = 0;
  int is_unsigned = 0;

  if (!name) {
    return 0;
  }
  if (strcmp(name, "int8") == 0) {
    bits = 8;
  } else if (strcmp(name, "uint8") == 0) {
    bits = 8;
    is_unsigned = 1;
  } else if (strcmp(name, "int16") == 0) {
    bits = 16;
  } else if (strcmp(name, "uint16") == 0) {
    bits = 16;
    is_unsigned = 1;
  } else if (strcmp(name, "int32") == 0) {
    bits = 32;
  } else if (strcmp(name, "uint32") == 0) {
    bits = 32;
    is_unsigned = 1;
  } else if (strcmp(name, "int64") == 0) {
    bits = 64;
  } else if (strcmp(name, "uint64") == 0) {
    bits = 64;
    is_unsigned = 1;
  } else {
    return 0;
  }

  if (bits_out) {
    *bits_out = bits;
  }
  if (is_unsigned_out) {
    *is_unsigned_out = is_unsigned;
  }
  return 1;
}

/* ---------------------------------------------------------------------------
 * Context: the per-function tables a query needs in O(1).
 * ------------------------------------------------------------------------- */

/* Declared integer types are cached as one int per symbol so the O(n) scan
 * over DECLARE_LOCAL runs once per pass, not once per query. */
#define VR_TYPE_ENCODE(bits, uns) (((long long)(bits) << 2) | ((uns) ? 2 : 0) | 1)
#define VR_TYPE_BITS(enc) ((int)((enc) >> 2))
#define VR_TYPE_UNSIGNED(enc) (((enc) & 2) != 0)

void ir_value_range_ctx_init(IRValueRangeCtx *ctx, const IRFunction *function) {
  if (!ctx) {
    return;
  }
  ctx->function = function;
  ctx->built = 0;
  ctx->ok = 0;
}

void ir_value_range_ctx_destroy(IRValueRangeCtx *ctx) {
  if (!ctx || !ctx->built) {
    return;
  }
  ir_temp_value_map_destroy(&ctx->decl_types);
  ir_temp_value_map_destroy(&ctx->addr_taken);
  ir_temp_value_map_destroy(&ctx->monotone);
  ir_temp_value_map_destroy(&ctx->label_guard);
  ctx->built = 0;
  ctx->ok = 0;
}

/* Built on the first query, so a function the rewrites never ask about pays
 * nothing. */
static int vr_ctx_populate(IRValueRangeCtx *ctx) {
  const IRFunction *fn = ctx->function;

  if (!ir_addr_taken_set_build(fn, &ctx->addr_taken)) {
    return 0;
  }
  for (size_t i = 0; i < fn->parameter_count; i++) {
    int bits = 0, uns = 0;
    if (!fn->parameter_names || !fn->parameter_names[i] ||
        !fn->parameter_types || !fn->parameter_types[i] ||
        !ir_int_type_name_info(fn->parameter_types[i], &bits, &uns)) {
      continue;
    }
    IROperand value = ir_operand_int(VR_TYPE_ENCODE(bits, uns));
    if (!ir_temp_value_map_set(&ctx->decl_types, fn->parameter_names[i],
                               &value)) {
      return 0;
    }
  }
  for (size_t i = 0; i < fn->instruction_count; i++) {
    const IRInstruction *in = &fn->instructions[i];
    int bits = 0, uns = 0;
    if (in->op != IR_OP_DECLARE_LOCAL || in->dest.kind != IR_OPERAND_SYMBOL ||
        !in->dest.name || !ir_int_type_name_info(in->text, &bits, &uns)) {
      continue;
    }
    IROperand value = ir_operand_int(VR_TYPE_ENCODE(bits, uns));
    if (!ir_temp_value_map_set(&ctx->decl_types, in->dest.name, &value)) {
      return 0;
    }
  }
  return 1;
}

static int vr_ctx_build(IRValueRangeCtx *ctx) {
  if (ctx->built) {
    return ctx->ok;
  }
  ctx->built = 1;
  ctx->ok = 0;
  if (!ctx->function) {
    return 0;
  }
  if (!ir_temp_value_map_init(&ctx->decl_types) ||
      !ir_temp_value_map_init(&ctx->addr_taken) ||
      !ir_temp_value_map_init(&ctx->monotone) ||
      !ir_temp_value_map_init(&ctx->label_guard)) {
    /* Destroying a map that init never touched is safe: init zeroes it before
     * it can fail, and destroy on a zeroed map is a no-op. */
    ir_value_range_ctx_destroy(ctx);
    ctx->built = 1;
    return 0;
  }
  if (!vr_ctx_populate(ctx)) {
    ir_value_range_ctx_destroy(ctx);
    ctx->built = 1;
    return 0;
  }

  ctx->ok = 1;
  return 1;
}

static void vr_declared_range(IRValueRangeCtx *ctx, const char *symbol,
                              IRIntRange *out) {
  vr_full(out);
  if (!symbol) {
    return;
  }
  const IROperand *enc = ir_temp_value_map_lookup(&ctx->decl_types, symbol);
  if (!enc || enc->kind != IR_OPERAND_INT) {
    return;
  }
  vr_of_int_type(VR_TYPE_BITS(enc->int_value),
                 VR_TYPE_UNSIGNED(enc->int_value), out);
}

static int vr_symbol_address_taken(IRValueRangeCtx *ctx, const char *symbol) {
  return symbol && ir_temp_value_map_lookup(&ctx->addr_taken, symbol) != NULL;
}

/* A local or parameter of this function whose address never escapes. Only such
 * a symbol keeps its value across a call, a store through a pointer, or inline
 * assembly; anything else (a global, or a local someone took `&` of) can be
 * rewritten by code the walk cannot see. Getting this wrong is not a missed
 * optimization but a miscompile: `var x = 5; bump(&x); if (x != 6)` would fold
 * the test against the stale 5. */
static int vr_symbol_is_private(IRValueRangeCtx *ctx, const char *symbol) {
  return symbol && ir_temp_value_map_lookup(&ctx->decl_types, symbol) != NULL &&
         !vr_symbol_address_taken(ctx, symbol);
}

static int vr_clobbers_memory(const IRInstruction *in) {
  return in->op == IR_OP_CALL || in->op == IR_OP_CALL_INDIRECT ||
         in->op == IR_OP_STORE || in->op == IR_OP_INLINE_ASM;
}

/* ---------------------------------------------------------------------------
 * The walk.
 * ------------------------------------------------------------------------- */

static void vr_operand_range(IRValueRangeCtx *ctx, size_t at,
                             const IROperand *operand, int depth,
                             IRIntRange *out);

/* Nearest writer of `name` (of kind `kind`) strictly before `at` in the same
 * straight-line region. The scan stops at a LABEL, so a hit is the reaching
 * definition on every path that reaches `at`. `stop_at_clobber` additionally
 * gives up at anything that could write memory, which is what a symbol the
 * function does not privately own needs. */
static int vr_find_block_writer(const IRFunction *fn, size_t at,
                                IROperandKind kind, const char *name,
                                int stop_at_clobber, size_t *out_index) {
  size_t scanned = 0;
  for (size_t i = at; i > 0 && scanned < VR_MAX_SCAN;) {
    i--;
    const IRInstruction *in = &fn->instructions[i];
    if (in->op == IR_OP_NOP) {
      continue;
    }
    scanned++;
    if (in->op == IR_OP_LABEL) {
      return 0;
    }
    if (in->dest.kind == kind && in->dest.name && name &&
        in->dest.name[0] == name[0] && strcmp(in->dest.name, name) == 0 &&
        ir_instruction_writes_destination(in)) {
      *out_index = i;
      return 1;
    }
    if (stop_at_clobber && vr_clobbers_memory(in)) {
      return 0;
    }
  }
  return 0;
}

/* Fold one relational fact `symbol <op> bound` (known TRUE) into `r`, where
 * `bound` is the range of whatever the symbol was compared against. A literal
 * is the degenerate case (`[c, c]`); comparing against another VALUE is just
 * as usable -- `i < n` with `n` a `uint16` still bounds `i` by 65534. Only the
 * endpoint that survives every value of the bound is taken. */
static void vr_apply_relation(const char *op, int symbol_on_left,
                              const IRIntRange *bound, IRIntRange *r) {
  int gt = 0, ge = 0, lt = 0, le = 0, eq = 0;

  if (strcmp(op, ">") == 0) {
    gt = 1;
  } else if (strcmp(op, ">=") == 0) {
    ge = 1;
  } else if (strcmp(op, "<") == 0) {
    lt = 1;
  } else if (strcmp(op, "<=") == 0) {
    le = 1;
  } else if (strcmp(op, "==") == 0) {
    eq = 1;
  } else {
    return;
  }

  if (!symbol_on_left) {
    /* `c < x` is `x > c`, and so on. */
    int t;
    t = gt; gt = lt; lt = t;
    t = ge; ge = le; le = t;
  }

  IRIntRange fact;
  vr_full(&fact);
  if (eq) {
    fact = *bound;
  } else if (gt) {
    if (bound->lo == VR_MAX) {
      return;
    }
    fact.lo = bound->lo + 1;
  } else if (ge) {
    fact.lo = bound->lo;
  } else if (lt) {
    if (bound->hi == VR_MIN) {
      return;
    }
    fact.hi = bound->hi - 1;
  } else if (le) {
    fact.hi = bound->hi;
  }
  vr_intersect(r, &fact);
}

static const char *vr_negate_relation(const char *op) {
  if (strcmp(op, ">") == 0) return "<=";
  if (strcmp(op, ">=") == 0) return "<";
  if (strcmp(op, "<") == 0) return ">=";
  if (strcmp(op, "<=") == 0) return ">";
  if (strcmp(op, "==") == 0) return "!=";
  if (strcmp(op, "!=") == 0) return "==";
  return NULL;
}

/* The branch that is the ONLY way into `label_index`, when that branch is a
 * `branch_zero`. Arriving through it means the condition that produced the
 * tested temp was FALSE, which is the other half of every `if`: the region
 * after `if (n < 0) { return ...; }` knows `n >= 0` even though no guard
 * appears in it. Returns the branch's instruction index, or SIZE_MAX.
 *
 * Both requirements are about there being no OTHER way in: the instruction
 * before the label must not fall through, and no jump or equality branch may
 * target it. Memoized per label -- the census is a whole-function scan. */
static size_t vr_label_entry_branch(IRValueRangeCtx *ctx, size_t label_index) {
  const IRFunction *fn = ctx->function;
  const char *label = fn->instructions[label_index].text;
  if (!label) {
    return (size_t)-1;
  }

  const IROperand *memo = ir_temp_value_map_lookup(&ctx->label_guard, label);
  if (memo && memo->kind == IR_OPERAND_INT) {
    return memo->int_value == 0 ? (size_t)-1 : (size_t)(memo->int_value - 1);
  }

  size_t result = (size_t)-1;
  int fallthrough_reaches = 1;
  for (size_t i = label_index; i > 0;) {
    i--;
    const IRInstruction *prev = &fn->instructions[i];
    if (prev->op == IR_OP_NOP) {
      continue;
    }
    fallthrough_reaches = !(prev->op == IR_OP_JUMP || prev->op == IR_OP_RETURN);
    break;
  }

  if (!fallthrough_reaches) {
    size_t sole = (size_t)-1;
    int usable = 1;
    for (size_t i = 0; i < fn->instruction_count && usable; i++) {
      const IRInstruction *in = &fn->instructions[i];
      if (!in->text || (in->op != IR_OP_JUMP && in->op != IR_OP_BRANCH_ZERO &&
                        in->op != IR_OP_BRANCH_EQ) ||
          strcmp(in->text, label) != 0) {
        continue;
      }
      if (in->op != IR_OP_BRANCH_ZERO || sole != (size_t)-1) {
        usable = 0;
        break;
      }
      sole = i;
    }
    if (usable) {
      result = sole;
    }
  }

  IROperand value =
      ir_operand_int(result == (size_t)-1 ? 0 : (long long)result + 1);
  ir_temp_value_map_set(&ctx->label_guard, label, &value);
  return result;
}

/* Fold the comparison behind a `branch_zero`'s tested temp into `r`. `arrived`
 * distinguishes the two edges out of the branch: 0 is the fall-through, where
 * the condition HELD, and 1 is the jump, where it did not (so the relation is
 * negated).
 *
 * Only a SIGNED comparison is read: an unsigned ordering proves an unsigned
 * fact, and reading it as a signed bound would invent facts for values above
 * LLONG_MAX. What the symbol is compared against may be any value, not just a
 * literal -- its own range supplies the bound. */
static void vr_apply_branch_fact(IRValueRangeCtx *ctx, size_t branch_index,
                                 const char *symbol, int arrived, int depth,
                                 IRIntRange *r) {
  const IRFunction *fn = ctx->function;
  const IRInstruction *branch = &fn->instructions[branch_index];
  if (branch->lhs.kind != IR_OPERAND_TEMP || !branch->lhs.name) {
    return;
  }

  size_t producer_index = 0;
  if (!vr_find_block_writer(fn, branch_index, IR_OPERAND_TEMP, branch->lhs.name,
                            0, &producer_index)) {
    return;
  }
  const IRInstruction *cmp = &fn->instructions[producer_index];
  if (cmp->op != IR_OP_BINARY || cmp->is_float || cmp->is_unsigned ||
      !cmp->text) {
    return;
  }
  /* The comparison must still describe the value the branch tested: a write to
   * the symbol between the two would make the bound describe a dead value. */
  for (size_t i = producer_index + 1; i < branch_index; i++) {
    const IRInstruction *in = &fn->instructions[i];
    if (ir_instruction_writes_destination(in) &&
        in->dest.kind == IR_OPERAND_SYMBOL && in->dest.name && symbol &&
        strcmp(in->dest.name, symbol) == 0) {
      return;
    }
  }

  const char *op = arrived ? vr_negate_relation(cmp->text) : cmp->text;
  if (!op) {
    return;
  }

  const IROperand *other = NULL;
  int symbol_on_left = 0;
  if (ir_operand_is_symbol_named(&cmp->lhs, symbol)) {
    other = &cmp->rhs;
    symbol_on_left = 1;
  } else if (ir_operand_is_symbol_named(&cmp->rhs, symbol)) {
    other = &cmp->lhs;
  } else {
    return;
  }

  IRIntRange bound;
  vr_operand_range(ctx, producer_index, other, depth + 1, &bound);
  if (vr_is_full(&bound)) {
    return;
  }
  vr_apply_relation(op, symbol_on_left, &bound, r);
}

/* Guards on the straight-line path into `at`: every `branch_zero %t, L` we
 * walk back over was NOT taken, so the condition that produced `%t` held.
 * The scan stops at the region's LABEL, at any write to `symbol`, and at any
 * call/store that could reach `symbol` through its address. */
static void vr_apply_guards(IRValueRangeCtx *ctx, size_t at, const char *symbol,
                            int depth, IRIntRange *r) {
  const IRFunction *fn = ctx->function;
  int private_local = vr_symbol_is_private(ctx, symbol);
  size_t scanned = 0;

  for (size_t i = at; i > 0 && scanned < VR_MAX_SCAN;) {
    i--;
    const IRInstruction *in = &fn->instructions[i];
    if (in->op == IR_OP_NOP) {
      continue;
    }
    scanned++;
    if (in->op == IR_OP_LABEL) {
      /* Start of the region. If the only way in is a branch that skipped to
       * here, its condition was FALSE on that edge -- the `else` half of the
       * fact the fall-through side gets for free. */
      size_t entry = vr_label_entry_branch(ctx, i);
      if (entry != (size_t)-1) {
        vr_apply_branch_fact(ctx, entry, symbol, 1, depth, r);
      }
      return;
    }
    if (ir_instruction_writes_destination(in) &&
        in->dest.kind == IR_OPERAND_SYMBOL && in->dest.name && symbol &&
        strcmp(in->dest.name, symbol) == 0) {
      return;
    }
    if (!private_local && vr_clobbers_memory(in)) {
      return;
    }
    if (in->op == IR_OP_BRANCH_ZERO) {
      vr_apply_branch_fact(ctx, i, symbol, 0, depth, r);
    }
  }
}

/* A monotone counter: every definition of `symbol` anywhere in the function is
 * either a non-negative constant or a step forward by a positive constant. Such
 * a value starts non-negative and only grows, so it is non-negative at every
 * program point -- the one escape being wraparound, which at 64 bits needs more
 * iterations than a machine can execute (a step of 1 would take on the order of
 * 2^63 of them). That is the ordinary no-overflow assumption every optimizer
 * makes about induction variables, and it is confined here to 64-bit signed
 * slots: a narrower slot wraps at its own width, which IS reachable, and a
 * narrower type is excluded below.
 *
 * The answer is memoized per symbol -- it is a whole-function property, so
 * recomputing it per query would make the walk quadratic. */
#define VR_IV_MAX_STEP (1ll << 20)

static int vr_symbol_is_monotone_counter(IRValueRangeCtx *ctx,
                                         const char *symbol) {
  if (!symbol) {
    return 0;
  }
  const IROperand *memo = ir_temp_value_map_lookup(&ctx->monotone, symbol);
  if (memo && memo->kind == IR_OPERAND_INT) {
    return memo->int_value != 0;
  }

  /* Only a 64-bit signed slot. An unsigned slot is already non-negative by
   * type, and a narrow slot wraps at a reachable iteration count. */
  const IROperand *enc = ir_temp_value_map_lookup(&ctx->decl_types, symbol);
  int result = 0;
  if (enc && enc->kind == IR_OPERAND_INT && VR_TYPE_BITS(enc->int_value) == 64 &&
      !VR_TYPE_UNSIGNED(enc->int_value)) {
    const IRFunction *fn = ctx->function;
    int saw_init = 0;
    result = 1;
    for (size_t i = 0; i < fn->instruction_count && result; i++) {
      const IRInstruction *in = &fn->instructions[i];
      if (!ir_instruction_writes_destination(in) ||
          in->dest.kind != IR_OPERAND_SYMBOL || !in->dest.name ||
          strcmp(in->dest.name, symbol) != 0) {
        continue;
      }
      if (in->is_float) {
        result = 0;
        break;
      }
      if (in->op == IR_OP_ASSIGN && in->lhs.kind == IR_OPERAND_INT &&
          in->lhs.int_value >= 0) {
        saw_init = 1;
        continue;
      }
      if (in->op == IR_OP_BINARY && in->text && strcmp(in->text, "+") == 0 &&
          ir_operand_is_symbol_named(&in->lhs, symbol) &&
          in->rhs.kind == IR_OPERAND_INT && in->rhs.int_value > 0 &&
          in->rhs.int_value <= VR_IV_MAX_STEP) {
        continue;
      }
      result = 0;
    }
    if (!saw_init) {
      result = 0;
    }
    /* A counter whose address escapes can be rewritten by a store we never
     * see, so the write census above is not a census at all. */
    if (result && vr_symbol_address_taken(ctx, symbol)) {
      result = 0;
    }
  }

  IROperand value = ir_operand_int(result ? 1 : 0);
  ir_temp_value_map_set(&ctx->monotone, symbol, &value);
  return result;
}

static int vr_add_ok(long long a, long long b, long long *out) {
  if ((b > 0 && a > VR_MAX - b) || (b < 0 && a < VR_MIN - b)) {
    return 0;
  }
  *out = a + b;
  return 1;
}

/* Multiply two NON-NEGATIVE bounds, refusing rather than overflowing.
 *
 * The check has to happen BEFORE the multiply. An after-the-fact `r / b != a`
 * test looks like it detects overflow and does not: signed overflow is
 * undefined, so an optimizing compiler may assume it did not happen and delete
 * the test outright -- which gcc did. The range that escaped was inverted
 * (`[0, -2199023255551]` for `acc * v17` over two 40-bit-masked values), and an
 * inverted range makes every comparison look decided, so `v17 < acc * v17`
 * folded to false and the program took the wrong branch. */
static int vr_mul_nonneg_ok(long long a, long long b, long long *out) {
  if (a < 0 || b < 0) {
    return 0;
  }
  if (a != 0 && b > VR_MAX / a) {
    return 0;
  }
  *out = a * b;
  return 1;
}

/* An inverted range means a bound was computed wrong; it can only be believed
 * by claiming facts about a value that has none. Fall back to "unknown". */
static void vr_normalize(IRIntRange *r) {
  if (r->lo > r->hi) {
    vr_full(r);
  }
}

static int vr_is_comparison(const char *op) {
  return strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 ||
         strcmp(op, "<") == 0 || strcmp(op, "<=") == 0 ||
         strcmp(op, ">") == 0 || strcmp(op, ">=") == 0 ||
         strcmp(op, "&&") == 0 || strcmp(op, "||") == 0;
}

/* Smallest all-ones mask that covers every value in [0, hi]. */
static unsigned long long vr_cover_mask(long long hi) {
  unsigned long long m = (unsigned long long)hi;
  m |= m >> 1;
  m |= m >> 2;
  m |= m >> 4;
  m |= m >> 8;
  m |= m >> 16;
  m |= m >> 32;
  return m;
}

static void vr_binary_range(IRValueRangeCtx *ctx, size_t index,
                            const IRInstruction *in, int depth,
                            IRIntRange *out) {
  const char *op = in->text;
  vr_full(out);
  if (!op) {
    return;
  }
  if (vr_is_comparison(op)) {
    out->lo = 0;
    out->hi = 1;
    return;
  }

  IRIntRange a, b;
  vr_operand_range(ctx, index, &in->lhs, depth + 1, &a);
  vr_operand_range(ctx, index, &in->rhs, depth + 1, &b);

  if (strcmp(op, "&") == 0) {
    /* Masking with a non-negative operand cannot produce more than that
     * operand, nor set the sign bit. */
    if (a.lo >= 0) {
      out->lo = 0;
      out->hi = a.hi;
    }
    if (b.lo >= 0 && (out->lo < 0 || b.hi < out->hi)) {
      out->lo = 0;
      out->hi = b.hi;
    }
    return;
  }
  if (strcmp(op, "|") == 0 || strcmp(op, "^") == 0) {
    if (a.lo >= 0 && b.lo >= 0) {
      unsigned long long m = vr_cover_mask(a.hi) | vr_cover_mask(b.hi);
      out->lo = 0;
      out->hi = (long long)m;
    }
    return;
  }
  if (strcmp(op, ">>") == 0) {
    /* Shifting is monotone, so the endpoints shift with the range. A negative
     * input is excluded because arithmetic and logical shift disagree there
     * and the IR does not always mark which one this is; a variable count is
     * excluded because `x >> n` with n unknown says nothing. */
    if (in->rhs.kind == IR_OPERAND_INT && in->rhs.int_value >= 0 &&
        in->rhs.int_value < 64 && a.lo >= 0) {
      out->lo = a.lo >> in->rhs.int_value;
      out->hi = a.hi >> in->rhs.int_value;
    }
    return;
  }
  if (strcmp(op, "<<") == 0) {
    if (in->rhs.kind == IR_OPERAND_INT && in->rhs.int_value >= 0 &&
        in->rhs.int_value < 63 && a.lo >= 0 &&
        a.hi <= (VR_MAX >> in->rhs.int_value)) {
      out->lo = a.lo << in->rhs.int_value;
      out->hi = a.hi << in->rhs.int_value;
    }
    return;
  }
  if (strcmp(op, "%") == 0) {
    /* Remainder of a non-negative dividend is non-negative and smaller than
     * the divisor's magnitude. */
    if (a.lo >= 0 && b.lo > 0) {
      out->lo = 0;
      out->hi = b.hi - 1 < a.hi ? b.hi - 1 : a.hi;
    }
    return;
  }
  if (strcmp(op, "/") == 0) {
    if (a.lo >= 0 && b.lo > 0) {
      out->lo = a.lo / b.hi;
      out->hi = a.hi / b.lo;
    }
    return;
  }
  if (strcmp(op, "+") == 0) {
    long long lo = 0, hi = 0;
    if (vr_add_ok(a.lo, b.lo, &lo) && vr_add_ok(a.hi, b.hi, &hi)) {
      out->lo = lo;
      out->hi = hi;
    }
    return;
  }
  if (strcmp(op, "-") == 0) {
    long long lo = 0, hi = 0;
    if (b.lo != VR_MIN && b.hi != VR_MIN && vr_add_ok(a.lo, -b.hi, &lo) &&
        vr_add_ok(a.hi, -b.lo, &hi)) {
      out->lo = lo;
      out->hi = hi;
    }
    return;
  }
  if (strcmp(op, "*") == 0) {
    long long lo = 0, hi = 0;
    if (a.lo >= 0 && b.lo >= 0 && vr_mul_nonneg_ok(a.lo, b.lo, &lo) &&
        vr_mul_nonneg_ok(a.hi, b.hi, &hi)) {
      out->lo = lo;
      out->hi = hi;
      vr_normalize(out);
    }
    return;
  }
}

static void vr_instruction_range(IRValueRangeCtx *ctx, size_t index, int depth,
                                 IRIntRange *out) {
  const IRInstruction *in = &ctx->function->instructions[index];
  vr_full(out);
  if (in->is_float) {
    return;
  }

  switch (in->op) {
  case IR_OP_ASSIGN:
    vr_operand_range(ctx, index, &in->lhs, depth + 1, out);
    break;
  case IR_OP_LOAD:
    /* A narrow load lands in the register extended by the pointee's own sign,
     * so the value is bounded by that type either way. rhs carries the access
     * size in bytes; an 8-byte load bounds nothing. */
    if (in->rhs.kind == IR_OPERAND_INT && in->rhs.int_value >= 1 &&
        in->rhs.int_value <= 4) {
      vr_of_int_type((int)(in->rhs.int_value * 8), in->is_unsigned, out);
    }
    break;
  case IR_OP_CAST: {
    int bits = 0, uns = 0;
    if (ir_int_type_name_info(in->text, &bits, &uns)) {
      IRIntRange target;
      vr_of_int_type(bits, uns, &target);
      vr_operand_range(ctx, index, &in->lhs, depth + 1, out);
      vr_narrow_to(out, &target);
    }
    break;
  }
  case IR_OP_BINARY:
    vr_binary_range(ctx, index, in, depth, out);
    break;
  case IR_OP_UNARY:
    if (in->text && strcmp(in->text, "!") == 0) {
      out->lo = 0;
      out->hi = 1;
    } else if (in->text && strcmp(in->text, "-") == 0) {
      IRIntRange a;
      vr_operand_range(ctx, index, &in->lhs, depth + 1, &a);
      /* Negating VR_MIN wraps back to itself, so a range that can reach it
       * says nothing. */
      if (a.lo > VR_MIN) {
        out->lo = -a.hi;
        out->hi = -a.lo;
      }
    }
    break;
  case IR_OP_SELECT: {
    /* dest = cond ? rhs : arguments[0] -- the result is one arm or the other,
     * so the hull of the two arms bounds it. */
    if (in->argument_count != 1) {
      break;
    }
    IRIntRange then_r, else_r;
    vr_operand_range(ctx, index, &in->rhs, depth + 1, &then_r);
    vr_operand_range(ctx, index, &in->arguments[0], depth + 1, &else_r);
    out->lo = then_r.lo < else_r.lo ? then_r.lo : else_r.lo;
    out->hi = then_r.hi > else_r.hi ? then_r.hi : else_r.hi;
    break;
  }
  default:
    break;
  }

  /* Writing through a typed slot wraps the value to that slot's width. */
  if (in->dest.kind == IR_OPERAND_SYMBOL && in->dest.name) {
    IRIntRange declared;
    vr_declared_range(ctx, in->dest.name, &declared);
    vr_narrow_to(out, &declared);
  }
}

static void vr_operand_range(IRValueRangeCtx *ctx, size_t at,
                             const IROperand *operand, int depth,
                             IRIntRange *out) {
  vr_full(out);
  if (!operand || depth >= VR_MAX_DEPTH) {
    return;
  }

  if (operand->kind == IR_OPERAND_INT) {
    out->lo = operand->int_value;
    out->hi = operand->int_value;
    return;
  }

  if (operand->kind == IR_OPERAND_TEMP && operand->name) {
    size_t producer = 0;
    if (vr_find_block_writer(ctx->function, at, IR_OPERAND_TEMP, operand->name,
                             0, &producer)) {
      vr_instruction_range(ctx, producer, depth, out);
    }
    return;
  }

  if (operand->kind != IR_OPERAND_SYMBOL || !operand->name) {
    return;
  }

  vr_declared_range(ctx, operand->name, out);

  int private_local = vr_symbol_is_private(ctx, operand->name);
  size_t writer = 0;
  if (vr_find_block_writer(ctx->function, at, IR_OPERAND_SYMBOL, operand->name,
                           !private_local, &writer)) {
    IRIntRange defined;
    vr_instruction_range(ctx, writer, depth, &defined);
    vr_intersect(out, &defined);
  }

  if (out->lo < 0 && vr_symbol_is_monotone_counter(ctx, operand->name)) {
    IRIntRange nonneg = {0, VR_MAX};
    vr_intersect(out, &nonneg);
  }

  vr_apply_guards(ctx, at, operand->name, depth, out);
  /* Normalizing at the shared recursion point stops a bad bound from
   * propagating up a chain, not just from reaching a consumer. */
  vr_normalize(out);
}

void ir_value_range_of(IRValueRangeCtx *ctx, size_t at, const IROperand *operand,
                       IRIntRange *out) {
  vr_full(out);
  if (!ctx || !out || !vr_ctx_build(ctx) ||
      at > ctx->function->instruction_count) {
    return;
  }
  vr_operand_range(ctx, at, operand, 0, out);
  /* Last line of defence: no consumer may ever see an inverted range, which
   * would read as "every fact holds". */
  vr_normalize(out);
}

int ir_value_is_nonnegative(IRValueRangeCtx *ctx, size_t at,
                            const IROperand *operand) {
  IRIntRange r;
  ir_value_range_of(ctx, at, operand, &r);
  return r.lo >= 0;
}

/* ---------------------------------------------------------------------------
 * The rewrites the ranges pay for.
 * ------------------------------------------------------------------------- */

static int vr_pow2_shift(long long value, long long *shift) {
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
  *shift = amount;
  return 1;
}

/* Fold a comparison whose operands' ranges already decide it. Both ranges must
 * be non-negative: signed and unsigned orderings agree there, so the fold is
 * correct whichever the instruction turns out to be at lowering time. */
static int vr_try_fold_comparison(IRValueRangeCtx *ctx, size_t at,
                                  IRInstruction *in, int *changed) {
  IRIntRange a, b;
  ir_value_range_of(ctx, at, &in->lhs, &a);
  if (a.lo < 0) {
    return 1;
  }
  ir_value_range_of(ctx, at, &in->rhs, &b);
  if (b.lo < 0) {
    return 1;
  }

  const char *op = in->text;
  int result = -1;
  if (strcmp(op, "<") == 0) {
    result = a.hi < b.lo ? 1 : (a.lo >= b.hi ? 0 : -1);
  } else if (strcmp(op, ">=") == 0) {
    result = a.hi < b.lo ? 0 : (a.lo >= b.hi ? 1 : -1);
  } else if (strcmp(op, ">") == 0) {
    result = a.lo > b.hi ? 1 : (a.hi <= b.lo ? 0 : -1);
  } else if (strcmp(op, "<=") == 0) {
    result = a.lo > b.hi ? 0 : (a.hi <= b.lo ? 1 : -1);
  } else if (strcmp(op, "==") == 0) {
    result = (a.hi < b.lo || a.lo > b.hi) ? 0 : -1;
  } else if (strcmp(op, "!=") == 0) {
    result = (a.hi < b.lo || a.lo > b.hi) ? 1 : -1;
  }
  if (result < 0) {
    return 1;
  }
  return ir_rewrite_to_assign_int(in, result, changed);
}

/* A branch whose ranges already decide it. Zero-ness and equality are the same
 * question signed or unsigned, so the signed bounds settle both. */
static int vr_try_resolve_branch(IRValueRangeCtx *ctx, size_t at,
                                 IRInstruction *in, int *changed) {
  IRIntRange a;

  if (in->op == IR_OP_BRANCH_ZERO) {
    if (in->lhs.kind == IR_OPERAND_INT) {
      return 1; /* the constant path already handles this */
    }
    ir_value_range_of(ctx, at, &in->lhs, &a);
    if (a.lo > 0 || a.hi < 0) {
      ir_instruction_make_nop(in); /* never zero: always falls through */
    } else if (a.lo == 0 && a.hi == 0) {
      ir_instruction_make_jump(in);
    } else {
      return 1;
    }
    if (changed) {
      *changed = 1;
    }
    return 1;
  }

  if (in->op == IR_OP_BRANCH_EQ) {
    if (in->lhs.kind == IR_OPERAND_INT && in->rhs.kind == IR_OPERAND_INT) {
      return 1;
    }
    IRIntRange b;
    ir_value_range_of(ctx, at, &in->lhs, &a);
    ir_value_range_of(ctx, at, &in->rhs, &b);
    if (a.hi < b.lo || b.hi < a.lo) {
      ir_instruction_make_nop(in); /* the values can never coincide */
      if (changed) {
        *changed = 1;
      }
    }
  }

  return 1;
}

int ir_value_range_simplify(IRValueRangeCtx *ctx, size_t at, IRInstruction *in,
                            int *changed) {
  if (!ctx || !in) {
    return 1;
  }
  if (in->op == IR_OP_BRANCH_ZERO || in->op == IR_OP_BRANCH_EQ) {
    return vr_try_resolve_branch(ctx, at, in, changed);
  }
  if (in->op != IR_OP_BINARY || in->is_float || !in->text) {
    return 1;
  }

  const char *op = in->text;
  int is_div = strcmp(op, "/") == 0;
  int is_mod = strcmp(op, "%") == 0;
  int is_and = strcmp(op, "&") == 0;

  if (is_div || is_mod) {
    /* The power-of-two cases are rows in the identity table
     * (ir_optimize_rewrite.c), which consults this analysis through the
     * P_NONNEG pattern. What is left here is the fold no pattern can express:
     * a divisor the dividend can never reach, whatever its shape. */
    if (in->rhs.kind != IR_OPERAND_INT || in->rhs.int_value <= 0) {
      return 1;
    }
    IRIntRange a;
    ir_value_range_of(ctx, at, &in->lhs, &a);
    if (a.lo < 0 || a.hi >= in->rhs.int_value) {
      return 1;
    }
    if (is_div) {
      return ir_rewrite_to_assign_int(in, 0, changed);
    }
    return ir_rewrite_to_assign_operand(in, &in->lhs, changed);
  }

  if (is_and) {
    const IROperand *value = NULL;
    long long mask = 0;
    if (in->rhs.kind == IR_OPERAND_INT && in->lhs.kind != IR_OPERAND_INT) {
      value = &in->lhs;
      mask = in->rhs.int_value;
    } else if (in->lhs.kind == IR_OPERAND_INT &&
               in->rhs.kind != IR_OPERAND_INT) {
      value = &in->rhs;
      mask = in->lhs.int_value;
    } else {
      return 1;
    }
    if (mask < 0) {
      return 1;
    }
    IRIntRange a;
    ir_value_range_of(ctx, at, value, &a);
    if (a.lo < 0) {
      return 1;
    }
    unsigned long long cover = vr_cover_mask(a.hi);
    /* A mask covering every bit the value can carry does nothing; a mask
     * sharing no bit with it produces nothing. */
    if ((cover & (unsigned long long)mask) == cover) {
      return ir_rewrite_to_assign_operand(in, value, changed);
    }
    if ((cover & (unsigned long long)mask) == 0ull) {
      return ir_rewrite_to_assign_int(in, 0, changed);
    }
    return 1;
  }

  if (strcmp(op, ">>") == 0 && in->rhs.kind == IR_OPERAND_INT &&
      in->rhs.int_value > 0 && in->rhs.int_value < 64) {
    /* Shifting every reachable bit off the bottom leaves zero. */
    IRIntRange a;
    ir_value_range_of(ctx, at, &in->lhs, &a);
    if (a.lo >= 0 && (a.hi >> in->rhs.int_value) == 0) {
      return ir_rewrite_to_assign_int(in, 0, changed);
    }
    return 1;
  }

  if (vr_is_comparison(op) && strcmp(op, "&&") != 0 && strcmp(op, "||") != 0) {
    /* Only worth a query when one side is a constant or a typed slot; a
     * comparison of two unbounded values can never be decided. */
    if (in->lhs.kind == IR_OPERAND_INT && in->rhs.kind == IR_OPERAND_INT) {
      return 1; /* already folded by the constant evaluator */
    }
    return vr_try_fold_comparison(ctx, at, in, changed);
  }

  return 1;
}

/* ---------------------------------------------------------------------------
 * `x % 2^k` asked only whether it is zero.
 *
 * This one is a USE-context rewrite, not a value rewrite, and it is why the
 * range analysis above does not subsume it: `x % 8` and `x & 7` are DIFFERENT
 * values when x is negative (-9 % 8 is -1, -9 & 7 is 7) and agree only on the
 * question "are the low k bits clear?". So when the remainder's single
 * consumer is a test against zero -- an explicit `== 0` / `!= 0`, or the
 * `branch_zero` a bare `if (x % 4)` lowers to -- the mask answers it for any
 * sign of x, and nothing has to be proven about the dividend at all.
 * ------------------------------------------------------------------------- */

/* Index of the temp operand that this instruction tests against zero, or NULL
 * when it is not a zero test. */
static const IROperand *vr_zero_test_operand(const IRInstruction *in) {
  if (in->op == IR_OP_BRANCH_ZERO && in->lhs.kind == IR_OPERAND_TEMP &&
      in->lhs.name) {
    return &in->lhs;
  }
  if (in->op != IR_OP_BINARY || in->is_float || !in->text ||
      (strcmp(in->text, "==") != 0 && strcmp(in->text, "!=") != 0)) {
    return NULL;
  }
  if (in->lhs.kind == IR_OPERAND_TEMP && in->lhs.name &&
      ir_operand_is_int_value(&in->rhs, 0)) {
    return &in->lhs;
  }
  if (in->rhs.kind == IR_OPERAND_TEMP && in->rhs.name &&
      ir_operand_is_int_value(&in->lhs, 0)) {
    return &in->rhs;
  }
  return NULL;
}

int ir_remainder_zero_test_to_mask_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }

  IRTempUseMap uses;
  if (!ir_temp_use_map_init(&uses)) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (!ir_collect_instruction_temp_uses(&uses, &function->instructions[i])) {
      ir_temp_use_map_destroy(&uses);
      return 0;
    }
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    const IROperand *tested = vr_zero_test_operand(&function->instructions[i]);
    if (!tested || ir_temp_use_map_get(&uses, tested->name) != 1) {
      continue;
    }

    size_t producer_index = 0;
    if (!vr_find_block_writer(function, i, IR_OPERAND_TEMP, tested->name, 0,
                              &producer_index)) {
      continue;
    }
    IRInstruction *producer = &function->instructions[producer_index];
    if (producer->op != IR_OP_BINARY || producer->is_float || !producer->text ||
        strcmp(producer->text, "%") != 0 ||
        producer->rhs.kind != IR_OPERAND_INT) {
      continue;
    }

    long long shift = 0;
    if (!vr_pow2_shift(producer->rhs.int_value, &shift) || shift < 1 ||
        shift > 62) {
      continue;
    }

    char *text = mettle_strdup("&");
    if (!text) {
      ir_temp_use_map_destroy(&uses);
      return 0;
    }
    mettle_free_string(producer->text);
    producer->text = text;
    producer->rhs.int_value = ((long long)1 << shift) - 1;
    if (changed) {
      *changed = 1;
    }
  }

  ir_temp_use_map_destroy(&uses);
  return 1;
}

/* ---- backend canonicalization oracle ------------------------------------ */
/* The MIR lowering keeps every narrow integer home canonical (sign- or
 * zero-extended to 64 bits) by re-extending after each write, because MIR
 * computes in 64 bits and an int32 add may mathematically overflow its type.
 * When the ranges of the operands prove the exact 64-bit result already fits
 * the home's width, the wrap can never happen, the 64-bit bits ARE the
 * canonical form, and the re-extension is pure cost -- one instruction per
 * loop-counter step in every int32-indexed loop. The backend cannot see IR
 * ranges, so it borrows this oracle through an opaque handle. */

void *ir_value_range_oracle_create(const IRFunction *function) {
  IRValueRangeCtx *ctx = (IRValueRangeCtx *)calloc(1, sizeof(IRValueRangeCtx));
  if (ctx) {
    ir_value_range_ctx_init(ctx, function);
  }
  return ctx;
}

void ir_value_range_oracle_destroy(void *oracle) {
  if (!oracle) {
    return;
  }
  ir_value_range_ctx_destroy((IRValueRangeCtx *)oracle);
  free(oracle);
}

static int vr_oracle_bounds_fit(long long lo, long long hi, int bits,
                                int is_unsigned) {
  if (bits <= 0 || bits >= 64) {
    return 0;
  }
  if (is_unsigned) {
    return lo >= 0 && hi <= (long long)((1ull << bits) - 1);
  }
  return lo >= -(1ll << (bits - 1)) && hi <= (1ll << (bits - 1)) - 1;
}

int ir_value_range_result_is_narrow(void *oracle, size_t at, int bits,
                                    int is_unsigned) {
  IRValueRangeCtx *ctx = (IRValueRangeCtx *)oracle;
  if (!ctx || !ctx->function || at >= ctx->function->instruction_count) {
    return 0;
  }
  const IRInstruction *in = &ctx->function->instructions[at];
  IRIntRange a, b;
  if (in->op == IR_OP_ASSIGN) {
    if (in->lhs.kind != IR_OPERAND_TEMP && in->lhs.kind != IR_OPERAND_SYMBOL &&
        in->lhs.kind != IR_OPERAND_INT) {
      return 0;
    }
    ir_value_range_of(ctx, at, &in->lhs, &a);
    return vr_oracle_bounds_fit(a.lo, a.hi, bits, is_unsigned);
  }
  if (in->op != IR_OP_BINARY || in->is_float || !in->text) {
    return 0;
  }
  int is_add = strcmp(in->text, "+") == 0;
  int is_sub = strcmp(in->text, "-") == 0;
  if (!is_add && !is_sub) {
    return 0;
  }
  ir_value_range_of(ctx, at, &in->lhs, &a);
  ir_value_range_of(ctx, at, &in->rhs, &b);
  /* Exact interval arithmetic; anything near the 64-bit edge is rejected so
   * the bound computation itself cannot wrap. */
  const long long LIM = 1ll << 62;
  if (a.lo <= -LIM || a.hi >= LIM || b.lo <= -LIM || b.hi >= LIM) {
    return 0;
  }
  long long lo = is_add ? a.lo + b.lo : a.lo - b.hi;
  long long hi = is_add ? a.hi + b.hi : a.hi - b.lo;
  return vr_oracle_bounds_fit(lo, hi, bits, is_unsigned);
}
