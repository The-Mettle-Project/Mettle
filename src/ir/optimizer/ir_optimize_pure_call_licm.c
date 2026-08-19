#include "ir_optimize_internal.h"
#include "../../common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Loop-invariant call hoisting: declared `@pure` and inferred read-only.
 *
 * `@pure` is a user contract asserting a function is free of side effects AND
 * safe to evaluate speculatively (it neither writes observable state nor
 * carries a fault that only matters when actually reached). Under that
 * contract a call to a pure function whose arguments do not change across a
 * loop returns the same value on every iteration, so we evaluate it once in the
 * loop preheader and reuse the result.
 *
 * Undecorated functions get the weaker inferred grade: a whole-program
 * fixpoint marks every function that provably writes nothing (no store, no
 * allocation, no global write, no unknown or indirect call, transitively).
 * Such a call is still not speculatable -- it may fault through a pointer
 * argument -- so its hoisted copy runs under a clone of the loop's entry test
 * and is skipped when the loop would not run; the guard makes the hoist exact,
 * not speculative.
 *
 * Soundness rules (all required before a call is hoisted):
 *   - the callee is a defined function carrying `@pure`;
 *   - every call argument is loop-invariant (no instruction anywhere in the
 *     loop body writes the named symbol/temp); and
 *   - the loop body contains only hoist-safe ops -- in particular NO memory
 *     STORE, allocation, inline-asm, indirect call, or call to a non-pure
 *     function. A pure callee may read memory through pointer args, so the
 *     no-store rule is what guarantees the pointed-to memory is identical at
 *     the preheader and on the first iteration.
 *
 * This runs program-level (it must resolve callees by name) after inlining and
 * before the per-function fixpoint, whose copy-propagation and dead-code
 * elimination then collapse the `dest <- %licm_pure_N` reduction left behind.
 * It is only reached under -O/--release (the optimize pipeline). */

static int g_pure_licm_counter;

static int pure_licm_is_simd_marker(const IRInstruction *inst) {
  return inst && inst->op == IR_OP_NOP && inst->text &&
         strncmp(inst->text, IR_SIMD_MARKER_PREFIX,
                 strlen(IR_SIMD_MARKER_PREFIX)) == 0;
}

/* A CALL to a `noreturn` runtime trap (the null / bounds / overflow guards that
 * pointer-indexing loops carry). It aborts the process rather than writing any
 * memory a pure callee could read, so its presence in the body does not block
 * hoisting a loop-invariant pure call: the guard is not moved and still fires
 * for the same inputs, while the hoisted call is safe to evaluate up front under
 * the `@pure` (side-effect-free + speculatable) contract. Without this, every
 * loop that dereferences a pointer under runtime checks would be ineligible. */
static int pure_licm_is_runtime_trap_call(const IRInstruction *inst) {
  return inst && inst->op == IR_OP_CALL && inst->text &&
         (strcmp(inst->text, "mettle_crash_trap_ex") == 0 ||
          strcmp(inst->text, "meth_runtime_debug_trap") == 0);
}

/* Does any instruction in [lo, hi) write an operand named `name` of `kind`? */
static int pure_licm_name_written(const IRFunction *function, size_t lo,
                                  size_t hi, const char *name,
                                  IROperandKind kind) {
  for (size_t k = lo; k < hi; k++) {
    const IRInstruction *inst = &function->instructions[k];
    if (ir_instruction_writes_destination(inst) && inst->dest.name &&
        inst->dest.kind == kind && strcmp(inst->dest.name, name) == 0) {
      return 1;
    }
  }
  return 0;
}

static int pure_licm_operand_invariant(const IRFunction *function, size_t lo,
                                       size_t hi, const IROperand *op) {
  if (!op) {
    return 1;
  }
  switch (op->kind) {
  case IR_OPERAND_INT:
  case IR_OPERAND_FLOAT:
  case IR_OPERAND_STRING:
  case IR_OPERAND_NONE:
    return 1;
  case IR_OPERAND_TEMP:
  case IR_OPERAND_SYMBOL:
    return op->name &&
           !pure_licm_name_written(function, lo, hi, op->name, op->kind);
  default:
    return 0; /* LABEL or anything unexpected: be conservative. */
  }
}

/* A write to a symbol that is neither a parameter nor a declared local of
 * `function` targets a global. A `@pure` callee may read globals, so a global
 * write inside the loop body -- or inside the callee itself -- can change what a
 * hoisted call observes; the arg-invariance test only covers the call's explicit
 * arguments. Treat such a write as a side effect. (A global write lowers to an
 * ASSIGN to a `@name` symbol, not an IR_OP_STORE, so the no-store rule below
 * would not catch it on its own -- this was a real `@pure`-LICM miscompile.) */
static int pure_licm_writes_global(const IRFunction *function,
                                   const IRInstruction *inst) {
  if (!ir_instruction_writes_symbol(inst) || !inst->dest.name) {
    return 0;
  }
  if (ir_function_symbol_is_parameter(function, inst->dest.name)) {
    return 0;
  }
  return ir_function_local_declared_type(function, inst->dest.name) == NULL;
}

/* ---- inferred read-only functions --------------------------------------- */

/* One body sweep of the read-only fixpoint: every instruction must be free of
 * observable writes -- no STORE, no allocation, no global write, no indirect
 * call, no call to an unknown/extern function, and every direct callee must
 * itself still hold the read-only bit (a function's own bit is set while it is
 * being examined, so direct and mutual recursion pass through here and only a
 * genuine write anywhere in the cycle strips it). Loads are fine: "read-only",
 * not "const". */
static int pure_licm_body_is_readonly(IRProgram *program,
                                      const IRFunction *function) {
  for (size_t k = 0; k < function->instruction_count; k++) {
    const IRInstruction *inst = &function->instructions[k];
    if (pure_licm_writes_global(function, inst)) {
      return 0;
    }
    switch (inst->op) {
    case IR_OP_NOP:
    case IR_OP_LABEL:
    case IR_OP_JUMP:
    case IR_OP_BRANCH_ZERO:
    case IR_OP_BRANCH_EQ:
    case IR_OP_DECLARE_LOCAL:
    case IR_OP_ASSIGN:
    case IR_OP_ADDRESS_OF:
    case IR_OP_LOAD:
    case IR_OP_BINARY:
    case IR_OP_UNARY:
    case IR_OP_ROTATE_ADD:
    case IR_OP_CAST:
    case IR_OP_RETURN:
      break;
    case IR_OP_CALL: {
      if (pure_licm_is_runtime_trap_call(inst)) {
        break;
      }
      IRFunction *callee =
          inst->text ? ir_program_find_function(program, inst->text) : NULL;
      if (!callee || !callee->is_readonly_inferred) {
        return 0;
      }
      break;
    }
    default:
      return 0;
    }
  }
  return 1;
}

/* Optimistic greatest-fixpoint over the program: start every defined function
 * read-only, strip the bit from any whose body disproves it, and repeat until
 * a full sweep strips nothing (each round must strip at least one function to
 * continue, so rounds are bounded by the function count and in practice by the
 * call-graph depth of the impurity). */
static void pure_licm_infer_readonly(IRProgram *program) {
  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *fn = program->functions[i];
    if (fn) {
      fn->is_readonly_inferred = 1;
    }
  }
  int changed = 1;
  while (changed) {
    changed = 0;
    for (size_t i = 0; i < program->function_count; i++) {
      IRFunction *fn = program->functions[i];
      if (!fn || !fn->is_readonly_inferred) {
        continue;
      }
      if (!pure_licm_body_is_readonly(program, fn)) {
        fn->is_readonly_inferred = 0;
        changed = 1;
      }
    }
  }
}

/* Every instruction in [lo, hi) of `function` must be free of side effects that
 * could perturb a pure callee's memory reads or be unsafe to evaluate once up
 * front: no memory STORE, allocation, inline-asm, indirect call, call to a
 * non-pure function, and no write to a global. A pure call is itself allowed (it
 * is what we may hoist, and a second pure call does not invalidate the first),
 * as is a noreturn trap guard. Used both for the caller's loop body and -- to
 * sanity-check the unverified `@pure` contract before trusting it -- for the
 * candidate callee's own body. */
static int pure_licm_range_side_effect_free(IRProgram *program,
                                            const IRFunction *function,
                                            size_t lo, size_t hi) {
  for (size_t k = lo; k < hi; k++) {
    const IRInstruction *inst = &function->instructions[k];
    if (pure_licm_writes_global(function, inst)) {
      return 0;
    }
    switch (inst->op) {
    case IR_OP_NOP:
    case IR_OP_LABEL:
    case IR_OP_JUMP:
    case IR_OP_BRANCH_ZERO:
    case IR_OP_BRANCH_EQ:
    case IR_OP_DECLARE_LOCAL:
    case IR_OP_ASSIGN:
    case IR_OP_ADDRESS_OF:
    case IR_OP_LOAD:
    case IR_OP_BINARY:
    case IR_OP_UNARY:
    case IR_OP_ROTATE_ADD:
    case IR_OP_CAST:
    case IR_OP_RETURN:
      break;
    case IR_OP_CALL: {
      if (pure_licm_is_runtime_trap_call(inst)) {
        break; /* noreturn abort guard: safe to leave in the body. */
      }
      IRFunction *callee =
          inst->text ? ir_program_find_function(program, inst->text) : NULL;
      if (!callee || (!callee->is_pure && !callee->is_readonly_inferred)) {
        return 0; /* impure or unresolved call: memory may change. */
      }
      break;
    }
    default:
      /* STORE / NEW / CALL_INDIRECT / INLINE_ASM / MEMCPY_INLINE / every SIMD
       * idiom: may write memory or otherwise be unsafe to speculate. */
      return 0;
    }
  }
  return 1;
}

/* The callee carries `@pure`, but that contract is unverified. Before relying on
 * it to lift a call out of a loop, confirm the callee's body has no observable
 * side effect of its own. This rejects a function mislabeled `@pure` that, e.g.,
 * mutates a global: hoisting its call would change how many times that effect
 * runs (a real miscompile). The check trusts nested `@pure` callees one level
 * deep (it does not recurse), matching the loop-body rule. */
static int pure_licm_callee_hoistable(IRProgram *program,
                                      const IRFunction *callee) {
  return callee && pure_licm_range_side_effect_free(
                       program, callee, 0, callee->instruction_count);
}

/* This loop's back-edge is the LAST `jump <loop_label>` in the function. Using
 * the last (not the first) jump guarantees [header, backedge) spans the whole
 * body even when a `continue` also jumps back to the header from the middle. */
static size_t pure_licm_find_backedge(const IRFunction *function,
                                      size_t header_index,
                                      const char *loop_label) {
  size_t backedge = (size_t)-1;
  for (size_t j = header_index + 1; j < function->instruction_count; j++) {
    const IRInstruction *p = &function->instructions[j];
    if (p->op == IR_OP_JUMP && p->text && strcmp(p->text, loop_label) == 0) {
      backedge = j;
    }
  }
  return backedge;
}

/* Is a label with this name defined inside [lo, hi] of `function`? */
static int pure_licm_label_inside(const IRFunction *function, size_t lo,
                                  size_t hi, const char *name) {
  for (size_t k = lo; k <= hi && k < function->instruction_count; k++) {
    const IRInstruction *inst = &function->instructions[k];
    if (inst->op == IR_OP_LABEL && inst->text &&
        strcmp(inst->text, name) == 0) {
      return 1;
    }
  }
  return 0;
}

/* The loop's entry-condition prefix: starting right after the header label, a
 * run of side-effect-free value ops and loop-exit branches (BRANCH_* whose
 * target label is not defined inside the loop). Returns the index of the LAST
 * exit branch of that run, or 0 for "no usable guard".
 *
 * The prefix must cover the WHOLE condition for the guard to be exact, so the
 * scan aborts (returns 0) if it meets a branch to a label inside the loop
 * before ending: that is either an `||` (whose exit branches continue past an
 * interior join, making any prefix guard weaker than loop entry) or a body
 * that opens with an `if` (where we cannot tell condition from body). Ending
 * at a non-value op (a call, store, jump, label -- the body's first real
 * statement) means every exit branch seen belongs to the condition.
 *
 * Cloning this prefix ahead of the header is safe: it holds no store, call, or
 * global write, and the header evaluates the same instructions with the same
 * inputs immediately after, whether or not the loop is entered. */
static size_t pure_licm_cond_prefix_end(const IRFunction *function,
                                        size_t header, size_t backedge) {
  size_t last_exit_branch = 0;
  size_t limit = header + 25;
  for (size_t k = header + 1; k < backedge; k++) {
    if (k > limit) {
      return 0; /* condition too long to clone: skip the guard. */
    }
    const IRInstruction *inst = &function->instructions[k];
    switch (inst->op) {
    case IR_OP_NOP:
    case IR_OP_ASSIGN:
    case IR_OP_LOAD:
    case IR_OP_BINARY:
    case IR_OP_UNARY:
    case IR_OP_ROTATE_ADD:
    case IR_OP_CAST:
    case IR_OP_ADDRESS_OF:
      continue;
    case IR_OP_BRANCH_ZERO:
    case IR_OP_BRANCH_EQ:
      if (!inst->text) {
        return 0;
      }
      if (pure_licm_label_inside(function, header, backedge, inst->text)) {
        return 0; /* `||` or leading `if`: guard would not match loop entry. */
      }
      last_exit_branch = k;
      continue;
    default:
      return last_exit_branch; /* body begins: condition fully covered. */
    }
  }
  return last_exit_branch;
}

/* Perform at most one hoist in `function`. Returns 1 if it hoisted (the caller
 * re-runs to expose further opportunities), 0 if nothing was hoisted. */
static int pure_licm_hoist_one(IRProgram *program, IRFunction *function) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *header = &function->instructions[i];
    if (header->op != IR_OP_LABEL || !header->text ||
        !ir_label_is_while_header(header->text)) {
      continue;
    }
    const char *loop_label = header->text;
    size_t backedge = pure_licm_find_backedge(function, i, loop_label);
    if (backedge == (size_t)-1 || backedge <= i + 1) {
      continue;
    }
    if (!pure_licm_range_side_effect_free(program, function, i + 1, backedge)) {
      continue;
    }

    for (size_t c = i + 1; c < backedge; c++) {
      IRInstruction *call = &function->instructions[c];
      if (call->op != IR_OP_CALL || !call->text) {
        continue;
      }
      if (call->dest.kind != IR_OPERAND_TEMP &&
          call->dest.kind != IR_OPERAND_SYMBOL) {
        continue; /* no reusable result to bind. */
      }
      IRFunction *callee = ir_program_find_function(program, call->text);
      if (!callee) {
        continue;
      }
      /* Two grades of eligibility. A `@pure` callee (still verified against
       * its body: a mislabeled global mutation must not change how often the
       * effect runs) hoists unconditionally under the contract's speculation
       * guarantee. An inferred read-only callee may fault (a load through a
       * pointer), so it hoists only under a clone of the loop's entry test;
       * a loop whose condition yields no usable guard keeps the call. */
      int unconditional =
          callee->is_pure && pure_licm_callee_hoistable(program, callee);
      size_t guard_end = 0;
      if (!unconditional) {
        if (!callee->is_readonly_inferred) {
          continue;
        }
        guard_end = pure_licm_cond_prefix_end(function, i, backedge);
        if (guard_end == 0 || guard_end >= c) {
          continue;
        }
      }
      int all_invariant = 1;
      for (size_t a = 0; a < call->argument_count; a++) {
        if (!pure_licm_operand_invariant(function, i + 1, backedge,
                                         &call->arguments[a])) {
          all_invariant = 0;
          break;
        }
      }
      if (!all_invariant) {
        continue;
      }

      char temp_name[32];
      snprintf(temp_name, sizeof(temp_name), "licm_pure_%d",
               g_pure_licm_counter++);
      char skip_name[48];
      snprintf(skip_name, sizeof(skip_name), "licm_skip_%d",
               g_pure_licm_counter - 1);

      /* 1. Clone the call for the preheader and retarget it to the temp. */
      IRInstruction hoisted;
      if (!ir_clone_instruction_plain(call, &hoisted)) {
        return 0;
      }
      ir_operand_destroy(&hoisted.dest);
      hoisted.dest = ir_operand_temp(temp_name);

      /* Callee name for the --explain remark, captured before the insert
       * below takes ownership of `hoisted`'s storage. */
      char hoisted_callee[128];
      snprintf(hoisted_callee, sizeof(hoisted_callee), "%s",
               hoisted.text ? hoisted.text : "?");

      /* 2. For a guarded hoist, clone the condition prefix [i+1, guard_end]
       * and retarget its exit branches at the skip label. All cloning happens
       * before any mutation: inserting reallocates the instruction array and
       * invalidates every pointer into it, and a half-done rewrite must not
       * leave the loop reading an undefined temp. */
      size_t guard_count = unconditional ? 0 : guard_end - i;
      IRInstruction *guard_clones = NULL;
      if (guard_count > 0) {
        guard_clones = calloc(guard_count, sizeof(IRInstruction));
        if (!guard_clones) {
          ir_instruction_destroy_storage(&hoisted);
          return 0;
        }
        for (size_t k = 0; k < guard_count; k++) {
          int ok = ir_clone_instruction_plain(&function->instructions[i + 1 + k],
                                              &guard_clones[k]);
          if (ok && (guard_clones[k].op == IR_OP_BRANCH_ZERO ||
                     guard_clones[k].op == IR_OP_BRANCH_EQ)) {
            char *copy = mettle_strdup(skip_name);
            if (copy) {
              mettle_free_string(guard_clones[k].text);
              guard_clones[k].text = copy;
            } else {
              ok = 0;
            }
          }
          if (!ok) {
            for (size_t d = 0; d <= k; d++) {
              ir_instruction_destroy_storage(&guard_clones[d]);
            }
            free(guard_clones);
            ir_instruction_destroy_storage(&hoisted);
            return 0;
          }
        }
      }

      /* 3. Rewrite the in-loop call into `dest <- %temp`, keeping dest. */
      IROperand dest_copy = ir_operand_copy(&call->dest);
      int saved_is_float = call->is_float;
      int saved_float_bits = call->float_bits;
      SourceLocation saved_loc = call->location;
      ir_instruction_destroy_storage(call);
      call->op = IR_OP_ASSIGN;
      call->dest = dest_copy;
      call->lhs = ir_operand_temp(temp_name);
      call->rhs = ir_operand_none();
      call->is_float = saved_is_float;
      call->float_bits = saved_float_bits;
      call->location = saved_loc;

      /* 4. Insert the preheader block: before the header label and before any
       * `@simd` begin-markers bracketing it, so nothing lands inside the
       * contract verifier's marked region. Unconditional hoists insert just
       * the call; guarded hoists insert
       *     <condition clone> ; call -> %temp ; label skip
       * Entering the loop means every cloned exit branch fell through, so the
       * temp is defined on every path that reads it; when the loop would not
       * run, the call is skipped, never speculated. */
      size_t insert_idx = i;
      while (insert_idx > 0 && pure_licm_is_simd_marker(
                                   &function->instructions[insert_idx - 1])) {
        insert_idx--;
      }
      for (size_t k = 0; k < guard_count; k++) {
        if (!ir_instruction_insert_move(function, insert_idx, &guard_clones[k])) {
          for (size_t d = k; d < guard_count; d++) {
            ir_instruction_destroy_storage(&guard_clones[d]);
          }
          free(guard_clones);
          ir_instruction_destroy_storage(&hoisted);
          return 0;
        }
        insert_idx++;
      }
      free(guard_clones);
      if (!ir_instruction_insert_move(function, insert_idx, &hoisted)) {
        ir_instruction_destroy_storage(&hoisted);
        return 0;
      }
      insert_idx++;
      if (guard_count > 0) {
        IRInstruction skip_label = {0};
        skip_label.op = IR_OP_LABEL;
        skip_label.location = saved_loc;
        skip_label.text = mettle_strdup(skip_name);
        if (!skip_label.text ||
            !ir_instruction_insert_move(function, insert_idx, &skip_label)) {
          ir_instruction_destroy_storage(&skip_label);
          return 0;
        }
      }
      if (ir_explain_enabled()) {
        char entity[160];
        snprintf(entity, sizeof(entity), "call to `%s`", hoisted_callee);
        ir_explain_remark(
            function->name, entity, saved_loc, 1,
            "hoisted out of the loop (runs once, not every iteration)",
            unconditional
                ? "`@pure` + loop-invariant arguments enable loop-invariant "
                  "code motion"
                : "the callee is inferred read-only (it writes nothing "
                  "anywhere it can reach) and every argument is "
                  "loop-invariant; the hoisted call runs under a copy of the "
                  "loop's entry test",
            NULL, NULL);
        ir_explain_remark_code("hoisted");
      }
      return 1;
    }
  }
  return 0;
}

int ir_hoist_pure_calls_pass(IRProgram *program, int *changed) {
  if (!program) {
    return 0;
  }
  pure_licm_infer_readonly(program);
  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *function = program->functions[i];
    if (!function) {
      continue;
    }
    while (pure_licm_hoist_one(program, function)) {
      if (changed) {
        *changed = 1;
      }
    }
  }
  return 1;
}
