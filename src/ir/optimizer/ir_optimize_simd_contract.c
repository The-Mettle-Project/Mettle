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

/* Other contract checkers (`@inline!`, `@noalloc`) report through the same
 * "user error, not ICE" channel `@simd!` uses. */
void ir_optimize_note_user_error(void) { g_simd_contract_user_error = 1; }

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

/* First vectorized instruction in (begin, end). With any_depth == 0 only ops
 * at this loop's own nesting level count (ops inside a nested marked loop
 * belong to that loop); with any_depth == 1 the whole region counts. Returns
 * NULL when nothing vectorized. */
static const IRInstruction *ir_region_vectorized_ins(const IRFunction *function,
                                                     size_t begin, size_t end,
                                                     int any_depth) {
  int depth = 0;
  for (size_t i = begin + 1; i < end; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (ir_instruction_is_simd_marker(instruction)) {
      depth += (instruction->text[strlen(IR_SIMD_MARKER_PREFIX)] == 'B') ? 1
                                                                         : -1;
      continue;
    }
    if ((any_depth || depth == 0) && ir_op_is_vectorized(instruction->op)) {
      return instruction;
    }
  }
  return NULL;
}

static int ir_region_vectorized_op(const IRFunction *function, size_t begin,
                                   size_t end) {
  const IRInstruction *ins = ir_region_vectorized_ins(function, begin, end, 0);
  return ins ? (int)ins->op : -1;
}

/* True when (begin, end) still contains a loop header label -- i.e. an actual
 * loop survived optimization. A region with markers but no loop label was
 * fully unrolled (constant trip count) or removed outright. */
static int ir_region_has_loop_label(const IRFunction *function, size_t begin,
                                    size_t end) {
  for (size_t i = begin + 1; i < end; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_LABEL && ins->text &&
        (strstr(ins->text, "ir_while_") != NULL ||
         strstr(ins->text, "ir_for_cond_") != NULL)) {
      return 1;
    }
  }
  return 0;
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
  int past_header = 0;
  for (size_t i = begin + 1; i < end; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_LABEL) {
      if (ins->text && (strstr(ins->text, "ir_while_") != NULL ||
                        strstr(ins->text, "ir_for_cond_") != NULL)) {
        past_header = 1;
      }
      continue;
    }
    /* Skip the once-only preamble between the begin marker and the header
     * label (a for-loop's initializer, hoisted pure calls): it is not the
     * loop and must not drive the diagnosis. */
    if (!past_header) {
      continue;
    }
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

/* Stable names for the IRSimdBailId schema (internal header). Used by future
 * structured output; kept in one place so the enum and names stay in sync. */
const char *ir_simd_bail_id_name(int id) {
  switch ((IRSimdBailId)id) {
  case IR_SIMD_BAIL_NONE:                return "none";
  case IR_SIMD_BAIL_CALL_IN_BODY:        return "call-in-body";
  case IR_SIMD_BAIL_INDIRECT_CALL:       return "indirect-call";
  case IR_SIMD_BAIL_ALLOC_IN_BODY:       return "alloc-in-body";
  case IR_SIMD_BAIL_INLINE_ASM:          return "inline-asm";
  case IR_SIMD_BAIL_CONTROL_FLOW:        return "control-flow";
  case IR_SIMD_BAIL_INT16_ELEMENTS:      return "int16-elements";
  case IR_SIMD_BAIL_INT64_ELEMENTS:      return "int64-elements";
  case IR_SIMD_BAIL_SERIAL_RECURRENCE:   return "serial-recurrence";
  case IR_SIMD_BAIL_MIXED_FLOAT_WIDTHS:  return "mixed-float-widths";
  case IR_SIMD_BAIL_BYTE_SUM_NARROW_ACC: return "byte-sum-narrow-acc";
  case IR_SIMD_BAIL_INLINED_PARAM_LOCAL: return "inlined-param-local";
  case IR_SIMD_BAIL_BODY_LOCAL:          return "body-local";
  case IR_SIMD_BAIL_DOT_SHAPE_ADDRESS:   return "dot-shape-address";
  case IR_SIMD_BAIL_STORE_ONLY_FILL:     return "store-only-fill";
  case IR_SIMD_BAIL_UNRECOGNIZED_SHAPE:  return "unrecognized-shape";
  }
  return "unknown";
}

#define IR_SIMD_SET_DIAG(value)                                                \
  do {                                                                         \
    if (diagnosis_out) {                                                       \
      *diagnosis_out = (value);                                                \
    }                                                                          \
  } while (0)

/* --explain: a deeper diagnosis than ir_simd_bail_reason, split into a reason
 * (what blocked vectorization), a fix (what the user can change), and a
 * machine-readable IRSimdBailId every branch must set. Best-effort but never
 * speculative: each claim is derived from instructions actually present in
 * the region. Empty fix = nothing actionable. */
static void ir_simd_explain_bail(const IRFunction *function, size_t begin,
                                 size_t end, char *reason, size_t reason_cap,
                                 char *fix, size_t fix_cap,
                                 int *diagnosis_out) {
  reason[0] = '\0';
  fix[0] = '\0';
  IR_SIMD_SET_DIAG(IR_SIMD_BAIL_UNRECOGNIZED_SHAPE);

  const char *callee = NULL;
  int has_indirect_call = 0, has_new = 0, has_asm = 0;
  int branch_count = 0, jump_count = 0;
  int has_i16 = 0, has_i64 = 0, has_f32 = 0, has_f64 = 0;
  int has_byte_load = 0, has_int_accum = 0;
  int has_float_accum = 0, has_float_mul = 0;
  int load_count = 0, store_count = 0;
  int past_header = 0; /* seen the loop's own header label yet? */
  const char *body_local = NULL;
  const char *recur_symbol = NULL;
  char recur_op = 0;

  for (size_t i = begin + 1; i < end; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_LABEL) {
      if (ins->text && (strstr(ins->text, "ir_while_") != NULL ||
                        strstr(ins->text, "ir_for_cond_") != NULL)) {
        past_header = 1;
      }
      continue;
    }
    /* The marker region starts BEFORE a for-loop's initializer, and hoisted
     * preamble code (pure-call LICM results, pointer setup) lands there too.
     * Everything before the header label runs ONCE -- it is not the loop, so
     * it must not drive the diagnosis. */
    if (!past_header) {
      continue;
    }
    switch (ins->op) {
    case IR_OP_DECLARE_LOCAL:
      /* A local declared INSIDE the loop body (after the header label -- a
       * range-for's induction local sits between the markers but BEFORE the
       * header and is fine) is one no recognizer's load->compute->store
       * matching can see through. The common source is an inlined callee's
       * parameter copy that copy-propagation couldn't fold (float32 narrowing
       * keeps the copy alive). */
      if (past_header && !body_local && ins->dest.kind == IR_OPERAND_SYMBOL &&
          ins->dest.name) {
        body_local = ins->dest.name;
      }
      break;
    case IR_OP_CALL:
      if (!(ins->text && strstr(ins->text, "crash_trap")) && !callee) {
        callee = ins->text ? ins->text : "?";
      }
      break;
    case IR_OP_CALL_INDIRECT:
      has_indirect_call = 1;
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
      if (ins->op == IR_OP_LOAD) {
        load_count++;
      } else {
        store_count++;
      }
      long long sz = (ins->rhs.kind == IR_OPERAND_INT) ? ins->rhs.int_value : 4;
      if (ins->is_float) {
        if (sz == 4) {
          has_f32 = 1;
        } else if (sz == 8) {
          has_f64 = 1;
        }
      } else {
        if (sz == 1 && ins->op == IR_OP_LOAD) {
          has_byte_load = 1;
        } else if (sz == 2) {
          has_i16 = 1;
        } else if (sz == 8) {
          has_i64 = 1;
        }
      }
      break;
    }
    case IR_OP_BINARY: {
      /* Integer '+' accumulation of a computed value into a symbol
       * (`total = total + %t`): together with byte loads this identifies the
       * vpsadbw byte-sum shape, whose kernel requires an int64 accumulator.
       * The added operand must be a temp -- `i = i + 1` (a constant) is an
       * induction variable, not a data sum. */
      if (!ins->is_float && ins->text && ins->text[0] == '+' &&
          !ins->text[1] && ins->dest.kind == IR_OPERAND_SYMBOL &&
          ins->dest.name &&
          ((ins->lhs.kind == IR_OPERAND_SYMBOL && ins->lhs.name &&
            strcmp(ins->lhs.name, ins->dest.name) == 0 &&
            ins->rhs.kind == IR_OPERAND_TEMP) ||
           (ins->rhs.kind == IR_OPERAND_SYMBOL && ins->rhs.name &&
            strcmp(ins->rhs.name, ins->dest.name) == 0 &&
            ins->lhs.kind == IR_OPERAND_TEMP))) {
        has_int_accum = 1;
      }
      /* Float multiply + float '+' accumulation = a dot-product-shaped
       * reduction (used below to give an address-pattern diagnosis when no
       * kernel claimed it). */
      if (ins->is_float && ins->text && ins->text[0] == '*' && !ins->text[1]) {
        has_float_mul = 1;
      }
      if (ins->is_float && ins->text && ins->text[0] == '+' &&
          !ins->text[1] &&
          ((ins->lhs.kind == IR_OPERAND_SYMBOL && ins->lhs.name) ||
           (ins->rhs.kind == IR_OPERAND_SYMBOL && ins->rhs.name))) {
        has_float_accum = 1;
      }
      /* Serial recurrence through a float symbol: `s = s * x` / `s = s / x`,
       * either directly (dest is the symbol) or via the usual temp+ASSIGN
       * pair. '+'/'-' accumulations are NOT flagged -- those reassociate and
       * have reduction kernels; '*' and '/' chains are genuinely serial. */
      if (recur_symbol || !ins->is_float || !ins->text ||
          (ins->text[0] != '*' && ins->text[0] != '/') || ins->text[1]) {
        break;
      }
      const char *lhs_sym =
          ins->lhs.kind == IR_OPERAND_SYMBOL ? ins->lhs.name : NULL;
      const char *rhs_sym =
          ins->rhs.kind == IR_OPERAND_SYMBOL ? ins->rhs.name : NULL;
      if (ins->dest.kind == IR_OPERAND_SYMBOL && ins->dest.name) {
        if ((lhs_sym && strcmp(lhs_sym, ins->dest.name) == 0) ||
            (rhs_sym && strcmp(rhs_sym, ins->dest.name) == 0)) {
          recur_symbol = ins->dest.name;
          recur_op = ins->text[0];
        }
      } else if (ins->dest.kind == IR_OPERAND_TEMP && ins->dest.name) {
        for (size_t j = i + 1; j < end && j < i + 9; j++) {
          const IRInstruction *later = &function->instructions[j];
          if (later->op == IR_OP_ASSIGN &&
              later->dest.kind == IR_OPERAND_SYMBOL && later->dest.name &&
              later->lhs.kind == IR_OPERAND_TEMP && later->lhs.name &&
              strcmp(later->lhs.name, ins->dest.name) == 0) {
            if ((lhs_sym && strcmp(lhs_sym, later->dest.name) == 0) ||
                (rhs_sym && strcmp(rhs_sym, later->dest.name) == 0)) {
              recur_symbol = later->dest.name;
              recur_op = ins->text[0];
            }
            break;
          }
        }
      }
      break;
    }
    default:
      break;
    }
  }

  if (callee) {
    snprintf(reason, reason_cap,
             "each iteration calls `%s`; loops vectorize only after every "
             "call in the body has been inlined away",
             callee);
    snprintf(fix, fix_cap,
             "make `%s` inline-eligible (small body, or mark it @inline), or "
             "hoist the call out of the loop",
             callee);
    IR_SIMD_SET_DIAG(IR_SIMD_BAIL_CALL_IN_BODY);
    return;
  }
  if (has_indirect_call) {
    snprintf(reason, reason_cap,
             "each iteration calls through a function pointer, which can "
             "never be inlined away");
    snprintf(fix, fix_cap,
             "call the target directly if it is known at compile time");
    IR_SIMD_SET_DIAG(IR_SIMD_BAIL_INDIRECT_CALL);
    return;
  }
  if (has_new) {
    snprintf(reason, reason_cap, "the loop body allocates memory (`new`) "
                                 "every iteration");
    snprintf(fix, fix_cap, "hoist the allocation out of the loop");
    IR_SIMD_SET_DIAG(IR_SIMD_BAIL_ALLOC_IN_BODY);
    return;
  }
  if (has_asm) {
    snprintf(reason, reason_cap,
             "the loop body contains inline assembly, which is opaque to the "
             "vectorizer");
    IR_SIMD_SET_DIAG(IR_SIMD_BAIL_INLINE_ASM);
    return;
  }
  if (branch_count > 1 || jump_count > 1) {
    snprintf(reason, reason_cap,
             "the loop body branches (an `if`, `&&`/`||`, or an early exit); "
             "only straight-line bodies vectorize");
    snprintf(fix, fix_cap,
             "compute both arms and select arithmetically (branchless), or "
             "split the work into two simpler loops");
    IR_SIMD_SET_DIAG(IR_SIMD_BAIL_CONTROL_FLOW);
    return;
  }
  if (has_i16) {
    snprintf(reason, reason_cap,
             "the loop reads/writes 16-bit integers, and no 16-bit kernels "
             "exist");
    snprintf(fix, fix_cap, "use int32 (or int8 if the values fit)");
    IR_SIMD_SET_DIAG(IR_SIMD_BAIL_INT16_ELEMENTS);
    return;
  }
  if (has_i64) {
    snprintf(reason, reason_cap,
             "the loop reads/writes 64-bit integer arrays, and no 64-bit "
             "integer kernels exist");
    snprintf(fix, fix_cap, "use int32 arrays if the values fit");
    IR_SIMD_SET_DIAG(IR_SIMD_BAIL_INT64_ELEMENTS);
    return;
  }
  if (recur_symbol) {
    snprintf(reason, reason_cap,
             "`%s` carries a serial `%c` recurrence -- every iteration needs "
             "the previous iteration's value, so lanes cannot run "
             "independently",
             recur_symbol, recur_op);
    snprintf(fix, fix_cap,
             "'+' reductions vectorize (they reassociate); serial '*'/'/' "
             "chains generally cannot -- if the recurrence is the point, this "
             "loop is at its scalar floor");
    IR_SIMD_SET_DIAG(IR_SIMD_BAIL_SERIAL_RECURRENCE);
    return;
  }
  if (has_f32 && has_f64) {
    snprintf(reason, reason_cap,
             "the loop mixes float32 and float64 elements; each kernel "
             "handles one width");
    snprintf(fix, fix_cap, "keep the loop in a single float width");
    IR_SIMD_SET_DIAG(IR_SIMD_BAIL_MIXED_FLOAT_WIDTHS);
    return;
  }
  if (has_byte_load && has_int_accum) {
    snprintf(reason, reason_cap,
             "this is a byte-sum loop, but the vpsadbw kernel accumulates "
             "into int64 and this loop's accumulator is narrower");
    snprintf(fix, fix_cap,
             "declare the accumulator as int64 (sum bytes as "
             "`total = total + (int64)data[i]`)");
    IR_SIMD_SET_DIAG(IR_SIMD_BAIL_BYTE_SUM_NARROW_ACC);
    return;
  }
  if (body_local) {
    if (strstr(body_local, "__inl_") != NULL) {
      snprintf(reason, reason_cap,
               "the body's data flow passes through the local `%s`, left over "
               "from an inlined call; the recognizers' "
               "load\xE2\x86\x92" "compute\xE2\x86\x92" "store matching "
               "cannot see through it",
               body_local);
      snprintf(fix, fix_cap,
               "a compiler limitation, not a code problem; write the "
               "expression directly in the loop body to vectorize today");
      IR_SIMD_SET_DIAG(IR_SIMD_BAIL_INLINED_PARAM_LOCAL);
    } else {
      snprintf(reason, reason_cap,
               "the body declares the local `%s` each iteration; the "
               "recognizers' load\xE2\x86\x92" "compute\xE2\x86\x92" "store "
               "matching cannot see through it",
               body_local);
      snprintf(fix, fix_cap,
               "declare `%s` before the loop, or fold the expression in "
               "directly",
               body_local);
      IR_SIMD_SET_DIAG(IR_SIMD_BAIL_BODY_LOCAL);
    }
    return;
  }
  if (has_float_mul && has_float_accum && load_count >= 2) {
    snprintf(reason, reason_cap,
             "this is a float multiply-accumulate (dot-product shape), but no "
             "kernel matched its address pattern -- the bases must be plain "
             "pointers indexed by the loop counter (base[i])");
    snprintf(fix, fix_cap,
             "hoist invariant index math into a pointer before the loop "
             "(e.g. `var row: float32* = &m[r * cols];` then `row[c]`)");
    IR_SIMD_SET_DIAG(IR_SIMD_BAIL_DOT_SHAPE_ADDRESS);
    return;
  }
  if (store_count > 0 && load_count == 0) {
    snprintf(reason, reason_cap,
             "the loop only writes (a fill/init pattern, no array reads); the "
             "recognizers cover maps, reductions, and dot products over "
             "loaded data, and no constant-fill kernel exists yet");
    IR_SIMD_SET_DIAG(IR_SIMD_BAIL_STORE_ONLY_FILL);
    return;
  }
  snprintf(reason, reason_cap, "no vectorizer recognized this loop's shape");
  snprintf(fix, fix_cap,
           "vectorizable shapes are unit-stride accesses (a[i], not a[i*k]) "
           "over int8/int32/float32/float64 with a straight-line body: maps "
           "(a[i] = expr), '+' reductions (s = s + expr), and dot products");
}

/* ---- fix hypothesis simulation ---------------------------------------------
 * "Verified" fix suggestions: apply the suggested source change as an
 * equivalent IR rewrite on a scratch clone, re-run the real vectorization
 * stages on it, and check whether a kernel claimed the loop. Only then does
 * the report print `verified: with that change ...` -- the claim is the
 * optimizer's own acceptance, not a prediction. */

/* Marker id of the loop beginning at `begin` (-1 when unparsable). The clone's
 * instruction indexes shift when passes rewrite it, so the loop is re-located
 * by this id afterwards. */
static int ir_simd_marker_id_at(const IRFunction *function, size_t begin) {
  const IRInstruction *marker = &function->instructions[begin];
  char which = 0;
  int id = 0, mode = 0;
  if (!ir_instruction_is_simd_marker(marker) ||
      sscanf(marker->text + strlen(IR_SIMD_MARKER_PREFIX), "%c:%d:%d", &which,
             &id, &mode) != 3) {
    return -1;
  }
  return id;
}

/* Find the B/E marker pair with `id` in `function`; returns 1 and fills the
 * region bounds on success. */
static int ir_simd_find_marker_region(const IRFunction *function, int id,
                                      size_t *begin_out, size_t *end_out) {
  size_t begin = (size_t)-1;
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    char which = 0;
    int marker_id = 0, mode = 0;
    if (!ir_instruction_is_simd_marker(ins) ||
        sscanf(ins->text + strlen(IR_SIMD_MARKER_PREFIX), "%c:%d:%d", &which,
               &marker_id, &mode) != 3 ||
        marker_id != id) {
      continue;
    }
    if (which == 'B') {
      begin = i;
    } else if (begin != (size_t)-1) {
      *begin_out = begin;
      *end_out = i;
      return 1;
    }
  }
  return 0;
}

/* A fix mutator applies one suggested source fix to the CLONE as the
 * equivalent IR rewrite, scoped to the loop region [begin, end]. Returns 1
 * when the rewrite was applied (the simulation may proceed), 0 when the
 * expected shape wasn't found (no claim is made). The clone is disposable:
 * it only has to convince the recognizers, not execute -- which is what
 * keeps mutators small. */
typedef int (*IRSimdFixMutator)(IRFunction *clone, size_t begin, size_t end);

/* Mutator for IR_SIMD_BAIL_BYTE_SUM_NARROW_ACC: widen the accumulator to
 * int64 the way the suggested source fix would -- retype its DECLARE_LOCAL
 * and retarget the byte-load's widening cast. */
static int ir_simd_mutate_byte_sum_int64(IRFunction *clone, size_t begin,
                                         size_t end) {
  int rewrote_cast = 0, rewrote_decl = 0;
  const char *acc_symbol = NULL;
  /* Locate the accumulation `S = S + %t` in the region. */
  for (size_t i = begin + 1; i < end && !acc_symbol; i++) {
    const IRInstruction *ins = &clone->instructions[i];
    if (ins->op == IR_OP_BINARY && !ins->is_float && ins->text &&
        strcmp(ins->text, "+") == 0 && ins->dest.kind == IR_OPERAND_SYMBOL &&
        ins->dest.name && ins->rhs.kind == IR_OPERAND_TEMP && ins->rhs.name &&
        ins->lhs.kind == IR_OPERAND_SYMBOL && ins->lhs.name &&
        strcmp(ins->lhs.name, ins->dest.name) == 0) {
      acc_symbol = ins->dest.name;
      /* The widening cast that produces %t, scanning backwards. */
      for (size_t j = i; j-- > begin;) {
        IRInstruction *cast = &clone->instructions[j];
        if (cast->op == IR_OP_CAST && cast->dest.kind == IR_OPERAND_TEMP &&
            cast->dest.name && strcmp(cast->dest.name, ins->rhs.name) == 0) {
          free(cast->text);
          cast->text = mettle_strdup("int64");
          rewrote_cast = cast->text != NULL;
          break;
        }
      }
    }
  }
  if (acc_symbol) {
    for (size_t i = 0; i < clone->instruction_count; i++) {
      IRInstruction *decl = &clone->instructions[i];
      if (decl->op == IR_OP_DECLARE_LOCAL &&
          decl->dest.kind == IR_OPERAND_SYMBOL && decl->dest.name &&
          strcmp(decl->dest.name, acc_symbol) == 0) {
        free(decl->text);
        decl->text = mettle_strdup("int64");
        rewrote_decl = decl->text != NULL;
        break;
      }
    }
  }
  return rewrote_cast && rewrote_decl;
}

/* The transform table: which diagnoses have a paired fix simulation. Growing
 * the hypothesis engine = adding a mutator and one row here. */
static const struct {
  IRSimdBailId diagnosis;
  IRSimdFixMutator mutate;
} g_simd_fix_transforms[] = {
    {IR_SIMD_BAIL_BYTE_SUM_NARROW_ACC, ir_simd_mutate_byte_sum_int64},
};

/* The shared simulation driver: clone the function, apply the mutator, re-run
 * the real optimization stages (remark recording suppressed), re-locate the
 * loop by marker id (indexes shift), and check whether a kernel claimed it.
 * On success fills `desc` with the kernel description and returns 1. */
static int ir_explain_simulate_fix(const IRFunction *function, size_t begin,
                                   size_t end, IRSimdFixMutator mutate,
                                   char *desc, size_t desc_cap) {
  int marker_id = ir_simd_marker_id_at(function, begin);
  if (marker_id < 0) {
    return 0;
  }

  IRFunction *clone = ir_explain_clone_function(function);
  if (!clone) {
    return 0;
  }
  if (!mutate(clone, begin, end)) {
    ir_function_destroy(clone);
    return 0;
  }

  ir_explain_set_hypothesis(1);
  int ran = ir_optimize_function_revectorize(clone);
  ir_explain_set_hypothesis(0);

  int verified = 0;
  size_t new_begin = 0, new_end = 0;
  if (ran && ir_simd_find_marker_region(clone, marker_id, &new_begin,
                                        &new_end)) {
    const IRInstruction *kernel =
        ir_region_vectorized_ins(clone, new_begin, new_end, 0);
    if (kernel) {
      ir_explain_kernel_desc(kernel, desc, desc_cap);
      verified = 1;
    }
  }
  ir_function_destroy(clone);
  return verified;
}

/* Run the fix simulation paired with `diagnosis`, if any. Returns 1 and fills
 * `desc` when the simulated fix was accepted by the optimizer. */
static int ir_explain_try_fix_for_diagnosis(const IRFunction *function,
                                            size_t begin, size_t end,
                                            int diagnosis, char *desc,
                                            size_t desc_cap) {
  size_t transform_count =
      sizeof(g_simd_fix_transforms) / sizeof(g_simd_fix_transforms[0]);
  for (size_t t = 0; t < transform_count; t++) {
    if ((int)g_simd_fix_transforms[t].diagnosis == diagnosis) {
      return ir_explain_simulate_fix(function, begin, end,
                                     g_simd_fix_transforms[t].mutate, desc,
                                     desc_cap);
    }
  }
  return 0;
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
#define IR_SIMD_MAX_LOOPS 256

/* One marker-bracketed loop region, collected during the contract walk so the
 * --explain pass can reason about nests (which loop contains which). */
typedef struct {
  size_t begin;
  size_t end;
  int mode;
  SourceLocation location;
} IRSimdLoopRecord;

/* --explain: one remark per recorded loop, nest-aware:
 *   - vectorized at its own level        -> "vectorized -> <kernel>"
 *   - scalar but a nested loop vectorized -> "vectorized inner, scalar outer"
 *   - scalar with a scalar nested loop    -> NOT vectorized (points inward)
 *   - no loop left between the markers    -> fully unrolled / removed
 *   - scalar leaf                         -> NOT vectorized + reason + fix */
static void ir_explain_report_loops(const IRFunction *function,
                                    const IRSimdLoopRecord *loops,
                                    size_t loop_count) {
  for (size_t k = 0; k < loop_count; k++) {
    const IRSimdLoopRecord *L = &loops[k];
    const IRInstruction *own =
        ir_region_vectorized_ins(function, L->begin, L->end, 0);
    const IRInstruction *any =
        own ? own : ir_region_vectorized_ins(function, L->begin, L->end, 1);

    int has_inner = 0;
    size_t inner_line = 0;
    for (size_t m = 0; m < loop_count; m++) {
      if (m == k || loops[m].begin <= L->begin || loops[m].end >= L->end) {
        continue;
      }
      has_inner = 1;
      if (inner_line == 0) {
        inner_line = loops[m].location.line;
      }
      /* Point the message at a vectorized inner loop when one exists. */
      if (!own && any &&
          ir_region_vectorized_ins(function, loops[m].begin, loops[m].end, 1)) {
        inner_line = loops[m].location.line;
      }
    }

    char headline[192], reason[320], fix[320];
    if (own) {
      char desc[128];
      ir_explain_kernel_desc(own, desc, sizeof(desc));
      snprintf(headline, sizeof(headline), "vectorized \xE2\x86\x92 %s", desc);
      ir_explain_remark(function->name, "loop", L->location, 1, headline, NULL,
                        NULL, NULL);
    } else if (any) {
      snprintf(reason, sizeof(reason),
               "only the innermost loop of a nest is vectorized; this loop "
               "drives the vectorized inner loop (line %zu)",
               inner_line);
      ir_explain_remark(function->name, "loop", L->location, 1,
                        "vectorized inner, scalar outer", reason, NULL, NULL);
    } else if (has_inner) {
      snprintf(reason, sizeof(reason),
               "the body contains a nested loop (line %zu), and only "
               "innermost loops are vectorized; the inner loop did not "
               "vectorize either -- see its remark",
               inner_line);
      ir_explain_remark(function->name, "loop", L->location, 0,
                        "NOT vectorized", reason, NULL, NULL);
    } else if (!ir_region_has_loop_label(function, L->begin, L->end)) {
      /* The unroller records a definitive "fully unrolled (N iterations)"
       * remark when it was the cause; only guess when nothing claimed it. */
      if (!ir_explain_has_remark_at(L->location.line, "loop")) {
        ir_explain_remark(function->name, "loop", L->location, 1,
                          "eliminated \xE2\x80\x94 no loop remains after "
                          "optimization (fully unrolled or folded away)",
                          NULL, NULL, NULL);
      }
    } else {
      int diagnosis = IR_SIMD_BAIL_NONE;
      ir_simd_explain_bail(function, L->begin, L->end, reason, sizeof(reason),
                           fix, sizeof(fix), &diagnosis);
      /* When the diagnosis has a paired hypothesis transform, simulate the
       * suggested fix and let the vectorizer itself confirm it. */
      char verified[320];
      verified[0] = '\0';
      char kernel_desc[128];
      if (ir_explain_try_fix_for_diagnosis(function, L->begin, L->end,
                                           diagnosis, kernel_desc,
                                           sizeof(kernel_desc))) {
        snprintf(verified, sizeof(verified),
                 "simulated that fix and re-ran the optimizer: this loop "
                 "then vectorizes \xE2\x86\x92 %s",
                 kernel_desc);
      }
      ir_explain_remark(function->name, "loop", L->location, 0,
                        "NOT vectorized", reason, fix[0] ? fix : NULL,
                        verified[0] ? verified : NULL);
    }
  }
}

int ir_verify_simd_contracts(IRFunction *function) {
  if (!function || function->instruction_count == 0) {
    return 1;
  }

  struct {
    int mode;
    size_t begin_index;
    SourceLocation location;
  } open[IR_SIMD_MAX_NESTING];
  IRSimdLoopRecord loops[IR_SIMD_MAX_LOOPS];
  size_t loop_count = 0;
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

    if (loop_count < IR_SIMD_MAX_LOOPS) {
      loops[loop_count].begin = begin_index;
      loops[loop_count].end = i;
      loops[loop_count].mode = loop_mode;
      loops[loop_count].location = loc;
      loop_count++;
    }

    if (loop_mode == SIMD_ATTR_REPORT) {
      continue; /* --explain bookkeeping only; no contract to enforce */
    }

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

  if (ir_explain_enabled()) {
    ir_explain_report_loops(function, loops, loop_count);
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
      if (!ir_instruction_is_simd_marker(&function->instructions[i])) {
        continue;
      }
      char which = 0;
      int id = 0, mode = 0;
      if (sscanf(function->instructions[i].text +
                     strlen(IR_SIMD_MARKER_PREFIX),
                 "%c:%d:%d", &which, &id, &mode) == 3 &&
          which == 'B' && mode != SIMD_ATTR_REPORT) {
        /* Report-only markers come from --explain, not from a user `@simd`;
         * they don't represent an unverified contract. */
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

  int contract_count = 0;
  for (size_t f = 0; f < program->function_count; f++) {
    IRFunction *function = program->functions[f];
    if (function && (function->is_inline_contract || function->is_noalloc)) {
      contract_count++;
    }
  }
  if (contract_count > 0) {
    fprintf(stderr,
            "note: %d `@inline!`/`@noalloc` contract%s present but not "
            "verified; contracts are only checked with -O/--release\n",
            contract_count, contract_count == 1 ? "" : "s");
  }
}
