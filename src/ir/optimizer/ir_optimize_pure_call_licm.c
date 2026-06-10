#include "ir_optimize_internal.h"

#include <stdio.h>
#include <string.h>

/* `@pure` loop-invariant call hoisting.
 *
 * `@pure` is a user contract asserting a function is free of side effects AND
 * safe to evaluate speculatively (it neither writes observable state nor
 * carries a fault that only matters when actually reached). Under that
 * contract a call to a pure function whose arguments do not change across a
 * loop returns the same value on every iteration, so we evaluate it once in the
 * loop preheader and reuse the result.
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
      if (!callee || !callee->is_pure) {
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
      if (!callee || !callee->is_pure) {
        continue;
      }
      /* Don't trust `@pure` blindly: if the callee's own body has a side
       * effect (notably a global mutation), hoisting changes how often it
       * runs. Keep the call in the loop. */
      if (!pure_licm_callee_hoistable(program, callee)) {
        continue;
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

      /* 2. Rewrite the in-loop call into `dest <- %temp`, keeping dest. */
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

      /* 3. Insert the hoisted call in the preheader: before the header label
       * and before any `@simd` begin-markers bracketing it, so the call never
       * lands inside the contract verifier's marked region. */
      size_t insert_idx = i;
      while (insert_idx > 0 && pure_licm_is_simd_marker(
                                   &function->instructions[insert_idx - 1])) {
        insert_idx--;
      }
      if (!ir_instruction_insert_move(function, insert_idx, &hoisted)) {
        ir_instruction_destroy_storage(&hoisted);
        return 0;
      }
      if (ir_explain_enabled()) {
        char entity[160];
        snprintf(entity, sizeof(entity), "call to `%s`", hoisted_callee);
        ir_explain_remark(
            function->name, entity, saved_loc, 1,
            "hoisted out of the loop (runs once, not every iteration)",
            "`@pure` + loop-invariant arguments enable loop-invariant code "
            "motion",
            NULL, NULL);
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
