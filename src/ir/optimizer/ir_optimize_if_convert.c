#include "ir_optimize_internal.h"

/* ---- If-conversion: data-dependent register diamond -> branchless select ----
 *
 * A lowered `if (c) { ... } else { ... }` whose two arms only compute and
 * assign registers (no loads, stores, calls, or nested control flow) becomes,
 * on unpredictable data, a branch the CPU keeps mispredicting. This pass
 * replaces such a diamond with straight-line code: both arms' pure
 * computations run unconditionally (sound -- no memory effects, no traps),
 * and every local the arms disagree on is reconciled with an IR_OP_SELECT
 * (lowered to a cmov). The classic case is a compare-select in a sort's
 * sift-down (`if (a[l] < a[r]) child = r`), where the loads sit in the guard
 * and the arm is a single register assignment.
 *
 * The lowered shape matched (contiguous, as ir_lowering emits it):
 *
 *     branch_zero %c -> ELSE          // then taken when %c != 0
 *       <then: pure register ops>
 *     jump JOIN
 *   ELSE:                             // label; else body may be empty
 *       <else: pure register ops>
 *   JOIN:                             // label
 *
 * Soundness restrictions (any violation just declines the rewrite):
 *   - each arm is straight-line, only ASSIGN / BINARY / UNARY / CAST, and the
 *     BINARY op is non-trapping (no / or %, which could fault when speculated);
 *   - a local is assigned at most once per arm and never read in the same arm
 *     (so hoisting the arm's temp computations unconditionally cannot observe
 *     a half-updated local -- the arm's net effect is a set of final values);
 *   - the arms are small (bounded), so speculating both is cheap.
 */

#define IR_IFCONV_MAX_ARM 10
#define IR_IFCONV_MAX_LOCALS 8

typedef struct {
  const char *name;  /* borrowed local symbol name */
  IROperand value;   /* the operand assigned to it in this arm (borrowed) */
} IRIfConvAssign;

typedef struct {
  size_t insns[IR_IFCONV_MAX_ARM]; /* pure temp-computing instruction indices */
  size_t insn_count;
  IRIfConvAssign locals[IR_IFCONV_MAX_LOCALS];
  size_t local_count;
} IRIfConvArm;

static int ir_ifconv_binary_is_trapping(const IRInstruction *in) {
  return in->text && (strcmp(in->text, "/") == 0 || strcmp(in->text, "%") == 0);
}

/* True if `in` writes a bare local symbol (an @sym destination). */
static int ir_ifconv_writes_local(const IRInstruction *in, const char **out) {
  if (in->dest.kind == IR_OPERAND_SYMBOL && in->dest.name) {
    *out = in->dest.name;
    return 1;
  }
  return 0;
}

static int ir_ifconv_arm_reads_local(const IRInstruction *in, const char *name) {
  if (ir_operand_is_symbol_named(&in->lhs, name) ||
      ir_operand_is_symbol_named(&in->rhs, name)) {
    return 1;
  }
  for (size_t a = 0; a < in->argument_count; a++) {
    if (ir_operand_is_symbol_named(&in->arguments[a], name)) {
      return 1;
    }
  }
  return 0;
}

/* Scan the arm [start, end) (end exclusive; the arm's terminator is NOT
 * included). Fill `arm` with its pure temp computations and final local
 * assignments. Returns 0 if the arm has any disallowed shape. */
static int ir_ifconv_scan_arm(const IRFunction *function, size_t start,
                              size_t end, IRIfConvArm *arm) {
  memset(arm, 0, sizeof(*arm));
  for (size_t i = start; i < end; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op == IR_OP_NOP || in->op == IR_OP_DECLARE_LOCAL) {
      continue;
    }
    if (in->op != IR_OP_ASSIGN && in->op != IR_OP_BINARY &&
        in->op != IR_OP_UNARY && in->op != IR_OP_CAST) {
      return 0; /* load/store/call/branch/... : not register-only */
    }
    if (in->op == IR_OP_BINARY && ir_ifconv_binary_is_trapping(in)) {
      return 0; /* div/mod can fault when speculated unconditionally */
    }

    const char *local = NULL;
    if (ir_ifconv_writes_local(in, &local)) {
      /* A local write must be a plain move `@L <- operand` (ASSIGN): the value
       * selected is exactly that operand. A cast-to-local would change width,
       * which a full-register cmov would not honor, so it is declined. */
      if (in->op != IR_OP_ASSIGN || in->is_float) {
        return 0;
      }
      if (in->lhs.kind != IR_OPERAND_TEMP && in->lhs.kind != IR_OPERAND_SYMBOL &&
          in->lhs.kind != IR_OPERAND_INT) {
        return 0;
      }
      /* One assignment per local; never re-read in the arm. */
      for (size_t k = 0; k < arm->local_count; k++) {
        if (strcmp(arm->locals[k].name, local) == 0) {
          return 0;
        }
      }
      if (arm->local_count >= IR_IFCONV_MAX_LOCALS) {
        return 0;
      }
      arm->locals[arm->local_count].name = local;
      arm->locals[arm->local_count].value = in->lhs;
      arm->local_count++;
      continue;
    }

    /* A temp computation: keep it to hoist unconditionally. */
    if (in->dest.kind != IR_OPERAND_TEMP || !in->dest.name) {
      return 0;
    }
    if (arm->insn_count >= IR_IFCONV_MAX_ARM) {
      return 0;
    }
    arm->insns[arm->insn_count++] = i;
  }

  /* No local assigned in the arm may be read anywhere in the arm (its temp
   * computations or other local sources). */
  for (size_t k = 0; k < arm->local_count; k++) {
    for (size_t i = start; i < end; i++) {
      if (ir_ifconv_arm_reads_local(&function->instructions[i],
                                    arm->locals[k].name)) {
        return 0;
      }
    }
  }
  return 1;
}

static const IROperand *ir_ifconv_arm_value(const IRIfConvArm *arm,
                                            const char *name) {
  for (size_t k = 0; k < arm->local_count; k++) {
    if (strcmp(arm->locals[k].name, name) == 0) {
      return &arm->locals[k].value;
    }
  }
  return NULL;
}

static int ir_ifconv_try_at(IRFunction *function, size_t branch_index,
                            int *changed) {
  const IRInstruction *branch = &function->instructions[branch_index];
  if (branch->op != IR_OP_BRANCH_ZERO || !branch->text ||
      (branch->lhs.kind != IR_OPERAND_TEMP &&
       branch->lhs.kind != IR_OPERAND_SYMBOL)) {
    return 1;
  }
  const char *else_label = branch->text;

  /* then block: [branch_index+1, then_jump). Ends with `jump JOIN`. */
  size_t then_jump = (size_t)-1;
  for (size_t i = branch_index + 1; i < function->instruction_count; i++) {
    IROpcode op = function->instructions[i].op;
    if (op == IR_OP_JUMP) {
      then_jump = i;
      break;
    }
    if (op == IR_OP_LABEL || op == IR_OP_BRANCH_ZERO || op == IR_OP_BRANCH_EQ ||
        op == IR_OP_RETURN) {
      return 1; /* not the clean then-block-then-jump shape */
    }
  }
  if (then_jump == (size_t)-1 || !function->instructions[then_jump].text) {
    return 1;
  }
  const char *join_label = function->instructions[then_jump].text;

  /* Right after the jump: `label ELSE`. */
  size_t else_label_index = then_jump + 1;
  while (else_label_index < function->instruction_count &&
         function->instructions[else_label_index].op == IR_OP_NOP) {
    else_label_index++;
  }
  if (else_label_index >= function->instruction_count ||
      function->instructions[else_label_index].op != IR_OP_LABEL ||
      !function->instructions[else_label_index].text ||
      strcmp(function->instructions[else_label_index].text, else_label) != 0) {
    return 1;
  }

  /* else block: [else_label_index+1, join_label_index). */
  size_t join_label_index = (size_t)-1;
  for (size_t i = else_label_index + 1; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op == IR_OP_LABEL) {
      if (in->text && strcmp(in->text, join_label) == 0) {
        join_label_index = i;
      }
      break; /* first label after the else body must be JOIN */
    }
  }
  if (join_label_index == (size_t)-1) {
    return 1;
  }

  IRIfConvArm then_arm, else_arm;
  if (!ir_ifconv_scan_arm(function, branch_index + 1, then_jump, &then_arm) ||
      !ir_ifconv_scan_arm(function, else_label_index + 1, join_label_index,
                          &else_arm)) {
    return 1;
  }
  if (then_arm.local_count == 0 && else_arm.local_count == 0) {
    return 1; /* nothing to select; leave the (empty) diamond to CFG cleanup */
  }

  /* Build the replacement sequence into a vector, then splice it in place of
   * [branch_index, join_label_index] (keeping the JOIN label). */
  IRInstructionVector seq = {0};
  int ok = 1;

  /* 1. Hoist both arms' pure temp computations (unconditional, sound). */
  const IRIfConvArm *arms[2] = {&then_arm, &else_arm};
  for (int a = 0; a < 2 && ok; a++) {
    for (size_t k = 0; k < arms[a]->insn_count && ok; k++) {
      IRInstruction cloned = {0};
      if (!ir_clone_instruction_plain(
              &function->instructions[arms[a]->insns[k]], &cloned) ||
          !ir_instruction_vector_append_move(&seq, &cloned)) {
        ir_instruction_destroy_storage(&cloned);
        ok = 0;
      }
    }
  }

  /* 2. One SELECT per local either arm assigns. A local absent from an arm
   * keeps its pre-diamond value on that side. */
  const char *seen[IR_IFCONV_MAX_LOCALS * 2];
  size_t seen_count = 0;
  for (int a = 0; a < 2 && ok; a++) {
    for (size_t k = 0; k < arms[a]->local_count && ok; k++) {
      const char *name = arms[a]->locals[k].name;
      int dup = 0;
      for (size_t s = 0; s < seen_count; s++) {
        if (strcmp(seen[s], name) == 0) {
          dup = 1;
          break;
        }
      }
      if (dup) {
        continue;
      }
      seen[seen_count++] = name;

      const IROperand *tv = ir_ifconv_arm_value(&then_arm, name);
      const IROperand *ev = ir_ifconv_arm_value(&else_arm, name);
      IROperand pre = ir_operand_symbol(name);
      IRInstruction sel = {0};
      sel.op = IR_OP_SELECT;
      sel.location = branch->location;
      sel.dest = ir_operand_symbol(name);
      sel.arguments = (IROperand *)calloc(1, sizeof(IROperand));
      /* dest = (cond != 0) ? then-value : else-value; a local absent from an
       * arm keeps its pre-diamond value. (A fused compare-select form -- cmp
       * folded in for `cmp; cmovcc` -- is a future optimization; see the pass
       * gate note. The interpreter/dump already accept it.) */
      int built = pre.name && sel.dest.name && sel.arguments &&
                  ir_operand_clone(&branch->lhs, &sel.lhs) &&
                  ir_operand_clone(tv ? tv : &pre, &sel.rhs) &&
                  ir_operand_clone(ev ? ev : &pre, &sel.arguments[0]);
      sel.argument_count = 1;
      ir_operand_destroy(&pre);
      if (!built || !ir_instruction_vector_append_move(&seq, &sel)) {
        ir_instruction_destroy_storage(&sel);
        ok = 0;
        break;
      }
    }
  }

  if (!ok) {
    ir_instruction_vector_destroy(&seq);
    return 0;
  }

  /* Rebuild: everything before the branch, the new sequence, then everything
   * from the JOIN label onward. */
  IRInstructionVector out = {0};
  for (size_t i = 0; i < branch_index && ok; i++) {
    IRInstruction c = {0};
    if (!ir_clone_instruction_plain(&function->instructions[i], &c) ||
        !ir_instruction_vector_append_move(&out, &c)) {
      ir_instruction_destroy_storage(&c);
      ok = 0;
    }
  }
  for (size_t k = 0; k < seq.count && ok; k++) {
    IRInstruction c = {0};
    if (!ir_clone_instruction_plain(&seq.items[k], &c) ||
        !ir_instruction_vector_append_move(&out, &c)) {
      ir_instruction_destroy_storage(&c);
      ok = 0;
    }
  }
  for (size_t i = join_label_index; i < function->instruction_count && ok; i++) {
    IRInstruction c = {0};
    if (!ir_clone_instruction_plain(&function->instructions[i], &c) ||
        !ir_instruction_vector_append_move(&out, &c)) {
      ir_instruction_destroy_storage(&c);
      ok = 0;
    }
  }
  ir_instruction_vector_destroy(&seq);
  if (!ok) {
    ir_instruction_vector_destroy(&out);
    return 0;
  }
  if (!ir_function_replace_instructions(function, &out)) {
    ir_instruction_vector_destroy(&out);
    return 0;
  }
  if (ir_explain_enabled()) {
    ir_explain_remark(function->name, "branch", branch->location, 1,
                      "if-converted to branchless select (cmov): a "
                      "data-dependent branch became straight-line code",
                      NULL, NULL, NULL);
  }
  if (changed) {
    *changed = 1;
  }
  return 1;
}

int ir_if_convert_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }
  /* OFF by default. Register-only if-conversion is NOT an unconditional win:
   * a cmov trades a (possibly well-predicted) branch for a data dependency,
   * and on a loop-carried critical path -- e.g. heapsort's sift-down, where
   * the selected index feeds the next iteration -- it measured SLOWER than the
   * branch. Turning this into a real win needs (a) a fused compare-select
   * (`cmp; cmovcc`, not the boolean-materializing `cmp; setcc; movzx; test;
   * cmov`) and (b) a predictability gate (branch is only worth converting when
   * it actually mispredicts -- the zero-run PGO interpreter can supply
   * per-branch taken counts). The IR_OP_SELECT + MIR_CMOV lowering below it is
   * correct and fuzz-clean; enable with METTLE_IF_CONVERT=1 to iterate. The
   * bigger cmov wins (merge/quicksort) need arm-store speculation this pass
   * does not yet do. */
  {
    static int enabled = -1;
    if (enabled < 0) {
      const char *e = getenv("METTLE_IF_CONVERT");
      enabled = e && *e && strcmp(e, "0") != 0;
    }
    if (!enabled) {
      return 1;
    }
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_BRANCH_ZERO) {
      int local_changed = 0;
      if (!ir_ifconv_try_at(function, i, &local_changed)) {
        return 0;
      }
      if (local_changed) {
        if (changed) {
          *changed = 1;
        }
        /* The stream was rebuilt; restart the scan from the top. */
        i = (size_t)-1;
      }
    }
  }
  return 1;
}
