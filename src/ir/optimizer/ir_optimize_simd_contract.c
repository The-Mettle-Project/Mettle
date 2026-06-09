#include "ir_optimize_internal.h"

#include <stdio.h>
#include <string.h>

/* Enforcement of the `@simd` / `@simd!` loop attributes.
 *
 * ir_lowering.c brackets each attributed loop with IR_OP_NOP markers whose
 * `text` is "@@simd:B:<id>:<mode>" / "@@simd:E:<id>:0" (see IR_SIMD_MARKER_PREFIX
 * in ir.h). By the time ir_verify_simd_contracts runs -- last in
 * ir_optimize_function_pipeline, after every vectorizer -- a loop a recognizer
 * accepted has been rewritten into a SIMD intrinsic op sitting between its
 * markers. So the contract test is simply: is there a vectorized op between B
 * and E?
 *
 *   @simd  (hint)     -> warn when not vectorized, keep the scalar loop
 *   @simd! (contract) -> hard compile error when not vectorized
 *
 * Enforcement only happens when optimization runs (-O / --release); plain debug
 * builds leave the markers as inert NOPs (ir_note_simd_contracts_unverified
 * prints one note explaining this and strips them). With --simd-report, every
 * `@simd` loop additionally reports what it became. */

/* Set when a `@simd!` contract is violated, so the driver can tell a user error
 * apart from an internal compiler error. */
static int g_simd_contract_user_error = 0;
/* --simd-report: emit a note for every `@simd` loop (vectorized or not). */
static int g_simd_report = 0;

void ir_optimize_reset_user_error(void) { g_simd_contract_user_error = 0; }

int ir_optimize_had_user_error(void) { return g_simd_contract_user_error; }

void ir_optimize_set_simd_report(int enabled) { g_simd_report = enabled; }

static int ir_instruction_is_simd_marker(const IRInstruction *instruction) {
  return instruction && instruction->op == IR_OP_NOP && instruction->text &&
         strncmp(instruction->text, IR_SIMD_MARKER_PREFIX,
                 strlen(IR_SIMD_MARKER_PREFIX)) == 0;
}

/* Any op in [IR_OP_COUNT_WORD_STARTS, IR_OP_SIMD_OUTER_LANE_F64] is one of the
 * accelerated idiom / SIMD intrinsics the recognizers emit; its presence means
 * the loop was claimed by a vectorizer. */
static int ir_op_is_vectorized(IROpcode op) {
  return op >= IR_OP_COUNT_WORD_STARTS && op <= IR_OP_SIMD_OUTER_LANE_F64;
}

/* If the loop body in (begin, end) was vectorized at this nesting depth (i.e.
 * the op is not one belonging to a nested `@simd` loop), return that opcode;
 * otherwise return -1. */
static int ir_region_vectorized_op(const IRFunction *function, size_t begin,
                                   size_t end) {
  int depth = 0;
  for (size_t i = begin + 1; i < end; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (ir_instruction_is_simd_marker(instruction)) {
      depth += (instruction->text[strlen(IR_SIMD_MARKER_PREFIX)] == 'B') ? 1
                                                                         : -1;
      continue;
    }
    if (depth == 0 && ir_op_is_vectorized(instruction->op)) {
      return (int)instruction->op;
    }
  }
  return -1;
}

/* A branch/jump whose target is one of the runtime-check labels the lowerer
 * injects (null-check, bounds-check). These appear per pointer/array access at
 * -O (they're absent at --release), so they must NOT count as user control flow
 * -- otherwise every loop that touches a pointer is misreported as having "its
 * own control flow". */
static int ir_label_is_runtime_check(const char *label) {
  if (!label) {
    return 0;
  }
  return strstr(label, "trap_null") != NULL || strstr(label, "nonnull") != NULL ||
         strstr(label, "trap_bounds") != NULL || strstr(label, "in_bounds") != NULL;
}

/* Best-effort explanation of why a loop the user marked `@simd` did not
 * vectorize, derived from the surviving scalar IR between the markers. A clean
 * counted loop has exactly one exit test (branch) and one back-edge (jump);
 * extras mean the body carries its own control flow (a nested loop or an `if`),
 * which the recognizers don't handle. */
static const char *ir_simd_bail_reason(const IRFunction *function, size_t begin,
                                       size_t end) {
  int has_call = 0, has_new = 0, has_asm = 0;
  int branch_count = 0, jump_count = 0;
  int has_i16 = 0, has_i64 = 0; /* unsupported memory element widths */
  for (size_t i = begin + 1; i < end; i++) {
    const IRInstruction *ins = &function->instructions[i];
    switch (ins->op) {
    case IR_OP_CALL:
    case IR_OP_CALL_INDIRECT:
      /* Ignore compiler-injected runtime-check traps (null/bounds checks emit
       * a guarded call to mettle_crash_trap_ex at -O; they're absent at
       * --release). Only user calls should drive the diagnosis. */
      if (!(ins->text && strstr(ins->text, "crash_trap"))) {
        has_call = 1;
      }
      break;
    case IR_OP_NEW:
      has_new = 1;
      break;
    case IR_OP_INLINE_ASM:
      has_asm = 1;
      break;
    case IR_OP_BRANCH_ZERO:
    case IR_OP_BRANCH_EQ:
      if (!ir_label_is_runtime_check(ins->text)) {
        branch_count++;
      }
      break;
    case IR_OP_JUMP:
      if (!ir_label_is_runtime_check(ins->text)) {
        jump_count++;
      }
      break;
    case IR_OP_LOAD:
    case IR_OP_STORE: {
      /* Vectorizable element widths: 1 (int8/uint8), 4 (int32/float32),
       * 8-float (float64). 16-bit ints and 64-bit ints have no kernel. The
       * load/store size lives in rhs; is_float distinguishes f64 from i64. */
      long long sz = (ins->rhs.kind == IR_OPERAND_INT) ? ins->rhs.int_value : 4;
      if (!ins->is_float) {
        if (sz == 2) {
          has_i16 = 1;
        } else if (sz == 8) {
          has_i64 = 1;
        }
      }
      break;
    }
    default:
      break;
    }
  }
  if (has_call) {
    return "the loop body contains a function call (only call-free or "
           "fully-inlined loops vectorize)";
  }
  if (has_new) {
    return "the loop body allocates memory (new)";
  }
  if (has_asm) {
    return "the loop body contains inline assembly";
  }
  if (branch_count > 1 || jump_count > 1) {
    return "the loop body has its own control flow (a nested loop or a "
           "data-dependent branch); only straight-line loop bodies vectorize";
  }
  if (has_i16) {
    return "the loop accesses 16-bit integers, which have no vectorizer "
           "(use int32/int8, or float32/float64)";
  }
  if (has_i64) {
    return "the loop accesses 64-bit integers, which have no vectorizer "
           "(use int32/int8, or float32/float64)";
  }
  /* Honest fallback: we've ruled out the disqualifiers we can detect, so the
   * truthful statement is that no kernel claimed this shape -- NOT an assertion
   * of a specific cause we haven't verified. */
  return "no vectorizer recognized this loop's shape (e.g. a non-unit stride, "
         "a loop-carried dependence, or a reduction/operation no kernel covers)";
}

static void ir_clear_simd_markers(IRFunction *function) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];
    if (ir_instruction_is_simd_marker(instruction)) {
      free(instruction->text);
      instruction->text = NULL; /* op stays IR_OP_NOP -- inert everywhere */
    }
  }
}

#define IR_SIMD_MAX_NESTING 64

int ir_verify_simd_contracts(IRFunction *function) {
  if (!function || function->instruction_count == 0) {
    return 1;
  }

  struct {
    int mode;
    size_t begin_index;
    SourceLocation location;
  } open[IR_SIMD_MAX_NESTING];
  int depth = 0;
  int had_fatal = 0;

  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (!ir_instruction_is_simd_marker(instruction)) {
      continue;
    }

    char which = 0;
    int id = 0, mode = 0;
    if (sscanf(instruction->text + strlen(IR_SIMD_MARKER_PREFIX), "%c:%d:%d",
               &which, &id, &mode) != 3) {
      continue;
    }

    if (which == 'B') {
      if (depth < IR_SIMD_MAX_NESTING) {
        open[depth].mode = mode;
        open[depth].begin_index = i;
        open[depth].location = instruction->location;
      }
      depth++;
      continue;
    }

    /* which == 'E' */
    if (depth <= 0) {
      continue; /* unbalanced; should not happen */
    }
    depth--;
    if (depth >= IR_SIMD_MAX_NESTING) {
      continue; /* its matching begin was past the nesting cap */
    }

    size_t begin_index = open[depth].begin_index;
    int loop_mode = open[depth].mode;
    SourceLocation loc = open[depth].location;
    const char *file = loc.filename ? loc.filename : "<input>";
    int vec_op = ir_region_vectorized_op(function, begin_index, i);

    if (vec_op >= 0) {
      if (g_simd_report) {
        fprintf(stderr, "%s:%zu:%zu: note: @simd loop vectorized (%s)\n", file,
                loc.line, loc.column, ir_opcode_name((IROpcode)vec_op));
      }
      continue; /* contract honored */
    }

    const char *reason = ir_simd_bail_reason(function, begin_index, i);
    if (loop_mode == SIMD_ATTR_CONTRACT) {
      fprintf(stderr, "%s:%zu:%zu: error: @simd! loop was not vectorized: %s\n",
              file, loc.line, loc.column, reason);
      g_simd_contract_user_error = 1;
      had_fatal = 1;
    } else {
      fprintf(stderr,
              "%s:%zu:%zu: warning: @simd loop was not vectorized: %s\n", file,
              loc.line, loc.column, reason);
    }
  }

  ir_clear_simd_markers(function);
  return had_fatal ? 0 : 1;
}

void ir_note_simd_contracts_unverified(IRProgram *program) {
  if (!program) {
    return;
  }
  int marker_count = 0;
  for (size_t f = 0; f < program->function_count; f++) {
    IRFunction *function = program->functions[f];
    if (!function) {
      continue;
    }
    for (size_t i = 0; i < function->instruction_count; i++) {
      if (ir_instruction_is_simd_marker(&function->instructions[i]) &&
          function->instructions[i].text[strlen(IR_SIMD_MARKER_PREFIX)] == 'B') {
        marker_count++;
      }
    }
    ir_clear_simd_markers(function);
  }
  if (marker_count > 0) {
    fprintf(stderr,
            "note: %d `@simd` loop%s present but not verified; vectorization "
            "contracts are only checked with -O/--release\n",
            marker_count, marker_count == 1 ? "" : "s");
  }
}
