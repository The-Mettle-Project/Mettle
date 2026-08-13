/* `--safe`: resolving the access marks lowering left behind.
 *
 * Every IR_OP_SAFETY_CHECK is either deleted, because the access provably
 * cannot leave its object, or rewritten into ordinary IR. Nothing downstream
 * sees the opcode. See ir_safety.h for why this runs before the optimizer. */

#include "ir_optimize_internal.h"
#include "../ir_explain_safety.h"
#include "../ir_safety.h"

/* Distinct from lowering's ".t%d" temps and from every label prefix the
 * recognizers match on, so a resolved check can never be mistaken for one. */
#define SAFETY_TEMP_PREFIX ".safe"
#define SAFETY_LABEL_PREFIX "ir_safe_ok_"

static unsigned g_safety_next_id;

/* METTLE_SAFETY_TRACE=1 prints why a proof gave up. A check that survives is
 * either a real limit of the analysis or a shape it should have recognized,
 * and from the outside those look identical: both are just a check that is
 * still there. Same purpose as the backend's mir_call_trace. */
static int safety_trace_enabled(void) {
  static int state = -1;
  if (state < 0) {
    const char *value = getenv("METTLE_SAFETY_TRACE");
    state = value && value[0] && value[0] != '0';
  }
  return state;
}

static void safety_trace(const char *reason, size_t line) {
  if (safety_trace_enabled()) {
    fprintf(stderr, "safety: line %zu unproven: %s\n", line, reason);
  }
}

typedef struct {
  const IROperand *base;
  const IROperand *offset;
  long long size;
  long long extent; /* IR_SAFETY_EXTENT_UNKNOWN when only the runtime knows */
  long long access_kind;
  const char *what;
  SourceLocation location;
} SafetyAccess;

/* Read a check's operands. Returns zero if the instruction is not shaped the
 * way lowering builds one, which leaves it to be copied through untouched
 * rather than silently mishandled. */
static int safety_read(const IRInstruction *instruction, SafetyAccess *access) {
  if (instruction->argument_count != IR_SAFETY_ARG_COUNT ||
      !instruction->arguments) {
    return 0;
  }
  const IROperand *size = &instruction->arguments[IR_SAFETY_ARG_SIZE];
  const IROperand *extent = &instruction->arguments[IR_SAFETY_ARG_EXTENT];
  const IROperand *kind = &instruction->arguments[IR_SAFETY_ARG_ACCESS];
  if (size->kind != IR_OPERAND_INT || extent->kind != IR_OPERAND_INT ||
      kind->kind != IR_OPERAND_INT) {
    return 0;
  }

  access->base = &instruction->arguments[IR_SAFETY_ARG_BASE];
  access->offset = &instruction->arguments[IR_SAFETY_ARG_OFFSET];
  access->size = size->int_value;
  access->extent = extent->int_value;
  access->access_kind = kind->int_value;
  access->what = instruction->text ? instruction->text : "?";
  access->location = instruction->location;
  return 1;
}

/* ---- emission helpers ------------------------------------------------------ */

static int safety_emit_binary(IRInstructionVector *out, SourceLocation location,
                              const char *op_text, const char *dest_temp,
                              const IROperand *lhs, const IROperand *rhs,
                              int is_unsigned) {
  IRInstruction insn = {0};
  insn.op = IR_OP_BINARY;
  insn.location = location;
  insn.text = mettle_strdup(op_text);
  insn.dest = ir_operand_temp(dest_temp);
  insn.is_unsigned = is_unsigned;
  if (!insn.text || !insn.dest.name || !ir_operand_clone(lhs, &insn.lhs) ||
      !ir_operand_clone(rhs, &insn.rhs) ||
      !ir_instruction_vector_append_move(out, &insn)) {
    ir_instruction_destroy_storage(&insn);
    return 0;
  }
  return 1;
}

static int safety_emit_branch_zero(IRInstructionVector *out,
                                   SourceLocation location,
                                   const char *condition_temp,
                                   const char *label) {
  IRInstruction insn = {0};
  insn.op = IR_OP_BRANCH_ZERO;
  insn.location = location;
  insn.text = mettle_strdup(label);
  insn.lhs = ir_operand_temp(condition_temp);
  if (!insn.text || !insn.lhs.name ||
      !ir_instruction_vector_append_move(out, &insn)) {
    ir_instruction_destroy_storage(&insn);
    return 0;
  }
  return 1;
}

static int safety_emit_label(IRInstructionVector *out, SourceLocation location,
                             const char *label) {
  IRInstruction insn = {0};
  insn.op = IR_OP_LABEL;
  insn.location = location;
  insn.text = mettle_strdup(label);
  if (!insn.text || !ir_instruction_vector_append_move(out, &insn)) {
    ir_instruction_destroy_storage(&insn);
    return 0;
  }
  return 1;
}

/* A call whose arguments are handed over by value. Each entry of `arguments`
 * is cloned, so the caller keeps ownership of what it passed in. */
static int safety_emit_call(IRInstructionVector *out, SourceLocation location,
                            const char *callee, const IROperand *arguments,
                            size_t count) {
  IRInstruction insn = {0};
  insn.op = IR_OP_CALL;
  insn.location = location;
  insn.text = mettle_strdup(callee);
  if (!insn.text) {
    ir_instruction_destroy_storage(&insn);
    return 0;
  }
  if (count > 0) {
    insn.arguments = calloc(count, sizeof(IROperand));
    if (!insn.arguments) {
      ir_instruction_destroy_storage(&insn);
      return 0;
    }
  }
  insn.argument_count = count;
  for (size_t i = 0; i < count; i++) {
    if (!ir_operand_clone(&arguments[i], &insn.arguments[i])) {
      ir_instruction_destroy_storage(&insn);
      return 0;
    }
  }
  if (!ir_instruction_vector_append_move(out, &insn)) {
    ir_instruction_destroy_storage(&insn);
    return 0;
  }
  return 1;
}

/* ---- proving a check cannot fail ------------------------------------------- */
/*
 * Deleting a check is a claim that the access can never leave its object, and
 * a wrong claim is a miscompile that reads as a safe program. So each proof
 * below establishes the whole range of offsets the access can take and
 * compares it against the extent; anything it cannot pin down exactly returns
 * zero and the check survives. Being wrong in that direction only costs
 * speed.
 */

/* Fold an operand to a constant by walking back through what produced it.
 *
 * Needed because lowering scales every subscript through a multiply into a
 * fresh temp, so even `a[3]` reaches the check as a temp rather than as the
 * twelve it obviously is. The walk is deliberately shallow: this runs before
 * the optimizer's constant folding, and its job is to see through lowering's
 * own scaffolding, not to re-implement that pass. */
static int safety_constant_value(const IRFunction *function, size_t before,
                                 const IROperand *operand, int depth,
                                 long long *out) {
  if (operand->kind == IR_OPERAND_INT) {
    *out = operand->int_value;
    return 1;
  }
  if (operand->kind != IR_OPERAND_TEMP || !operand->name || depth > 4) {
    return 0;
  }
  const IRInstruction *producer =
      ir_find_temp_producer_before(function, before, operand->name);
  if (!producer || producer->is_float) {
    return 0;
  }
  if (producer->op == IR_OP_ASSIGN) {
    return safety_constant_value(function, before, &producer->lhs, depth + 1,
                                 out);
  }
  if (producer->op != IR_OP_BINARY || !producer->text) {
    return 0;
  }
  long long lhs = 0;
  long long rhs = 0;
  if (!safety_constant_value(function, before, &producer->lhs, depth + 1,
                             &lhs) ||
      !safety_constant_value(function, before, &producer->rhs, depth + 1,
                             &rhs)) {
    return 0;
  }
  /* Only the operators lowering uses to build an offset, and only where the
   * result cannot overflow into a different answer than the machine gives. */
  if (strcmp(producer->text, "*") == 0) {
    if (lhs != 0 && (lhs > INT32_MAX || lhs < INT32_MIN || rhs > INT32_MAX ||
                     rhs < INT32_MIN)) {
      return 0;
    }
    *out = lhs * rhs;
    return 1;
  }
  if (strcmp(producer->text, "+") == 0) {
    *out = lhs + rhs;
    return 1;
  }
  if (strcmp(producer->text, "-") == 0) {
    *out = lhs - rhs;
    return 1;
  }
  return 0;
}

/* The offset is a constant the compiler already holds. */
static int safety_prove_constant(const IRFunction *function, size_t check_index,
                                 const SafetyAccess *access) {
  if (access->extent == IR_SAFETY_EXTENT_UNKNOWN) {
    return 0;
  }
  long long offset = 0;
  if (!safety_constant_value(function, check_index, access->offset, 0,
                             &offset)) {
    return 0;
  }
  if (offset < 0 || access->size <= 0 || access->size > access->extent) {
    return 0;
  }
  return offset <= access->extent - access->size;
}

/* Read `iv * stride` out of the instruction that produced the offset. This is
 * the shape lowering emits for every subscript: the index, then a multiply by
 * the element width. */
static int safety_offset_is_scaled_symbol(const IRFunction *function,
                                          size_t check_index,
                                          const IROperand *offset,
                                          const char **iv_out,
                                          long long *stride_out) {
  if (offset->kind != IR_OPERAND_TEMP || !offset->name) {
    return 0;
  }
  const IRInstruction *producer =
      ir_find_temp_producer_before(function, check_index, offset->name);
  if (!producer || producer->op != IR_OP_BINARY || producer->is_float ||
      !producer->text || strcmp(producer->text, "*") != 0 ||
      producer->lhs.kind != IR_OPERAND_SYMBOL || !producer->lhs.name ||
      producer->rhs.kind != IR_OPERAND_INT || producer->rhs.int_value <= 0) {
    return 0;
  }
  *iv_out = producer->lhs.name;
  *stride_out = producer->rhs.int_value;
  return 1;
}

/* Whether `symbol` is written anywhere in [start, end) outside the step's own
 * instructions. Used to confirm the only thing moving an induction variable
 * inside its loop is the step itself. */
static int safety_symbol_written_between(const IRFunction *function,
                                         size_t start, size_t end,
                                         const char *symbol, size_t step_first,
                                         size_t step_last) {
  for (size_t i = start; i < end && i < function->instruction_count; i++) {
    if (i >= step_first && i <= step_last) {
      continue;
    }
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->dest.kind == IR_OPERAND_SYMBOL &&
        instruction->dest.name &&
        strcmp(instruction->dest.name, symbol) == 0) {
      return 1;
    }
  }
  return 0;
}

/* `<temp> = iv + 1`, the first half of lowering's unfolded step. */
static int safety_is_add_one(const IRInstruction *instruction,
                             const char *iv) {
  return instruction && instruction->op == IR_OP_BINARY &&
         !instruction->is_float && instruction->text &&
         strcmp(instruction->text, "+") == 0 &&
         ir_operand_is_symbol_named(&instruction->lhs, iv) &&
         ir_operand_is_int_value(&instruction->rhs, 1);
}

/* Confirm the loop advances its index by exactly one, and report which
 * instructions do it.
 *
 * Two shapes, because this pass runs before the optimizer: lowering emits the
 * step as a pair, `t = i + 1` followed by `i = t`, and copy propagation folds
 * that into the single `i = i + 1` every recognizer downstream expects. Both
 * mean the same thing and both have to be read here. */
static int safety_loop_steps_by_one(const IRFunction *function,
                                    const IRWhileLoopBounds *loop,
                                    const char *iv, size_t *step_first,
                                    size_t *step_last) {
  size_t last = loop->jump_index;
  while (last > loop->branch_index + 1) {
    last--;
    if (function->instructions[last].op != IR_OP_NOP) {
      break;
    }
  }
  if (last <= loop->branch_index) {
    return 0;
  }

  const IRInstruction *tail = &function->instructions[last];
  if (ir_try_parse_direct_unit_increment(tail, iv)) {
    *step_first = last;
    *step_last = last;
    return 1;
  }

  if (tail->op != IR_OP_ASSIGN ||
      !ir_operand_is_symbol_named(&tail->dest, iv) ||
      tail->lhs.kind != IR_OPERAND_TEMP || !tail->lhs.name) {
    return 0;
  }
  const IRInstruction *add =
      ir_find_temp_producer_before(function, last, tail->lhs.name);
  if (!safety_is_add_one(add, iv)) {
    return 0;
  }
  *step_first = (size_t)(add - function->instructions);
  *step_last = last;
  return 1;
}

/* The check sits in a counted loop whose trip count and the object's extent
 * are both compile time constants, so the largest offset the loop can reach is
 * one too.
 *
 * Every condition here is load bearing. The variable has to start at zero and
 * only ever step by one, or the offsets it visits are not the range this
 * assumes; nothing else in the body may move it, or the increment is not the
 * whole story; and the bound has to be a constant, or there is no largest
 * offset to compare against. */
static int safety_prove_loop_bound(IRFunction *function, size_t check_index,
                                   const SafetyAccess *access) {
  if (access->extent == IR_SAFETY_EXTENT_UNKNOWN || access->size <= 0) {
    return 0;
  }

  const char *iv = NULL;
  long long stride = 0;
  if (!safety_offset_is_scaled_symbol(function, check_index, access->offset,
                                      &iv, &stride)) {
    safety_trace("the offset is not an index scaled by a constant width",
                 access->location.line);
    return 0;
  }

  /* Innermost enclosing loop stepping this variable. Scanning outward matters:
   * a check in a nested loop is indexed by the inner variable, and the outer
   * loop's bound says nothing about it. */
  for (size_t header = check_index; header-- > 0;) {
    if (function->instructions[header].op != IR_OP_LABEL ||
        !ir_label_is_while_header(function->instructions[header].text)) {
      continue;
    }
    IRWhileLoopBounds loop;
    if (!ir_find_while_loop_bounds(function, header, &loop) ||
        loop.jump_index == (size_t)-1) {
      continue;
    }
    if (check_index <= loop.branch_index || check_index >= loop.jump_index) {
      continue; /* the check is not in this loop's body */
    }

    const IRInstruction *compare = &function->instructions[loop.compare_index];
    if (!compare->lhs.name || strcmp(compare->lhs.name, iv) != 0) {
      continue; /* this loop steps a different variable; keep looking outward */
    }
    long long bound = 0;
    if (!safety_constant_value(function, loop.compare_index, &compare->rhs, 0,
                               &bound)) {
      safety_trace("the loop bound is not a constant", access->location.line);
      return 0;
    }
    if (bound <= 0) {
      return 1; /* the body never runs, so the access never happens */
    }

    if (!ir_iv_zero_at_header(function, header, iv)) {
      safety_trace("the loop index does not start at zero",
                   access->location.line);
      return 0;
    }
    size_t step_first = 0;
    size_t step_last = 0;
    if (!safety_loop_steps_by_one(function, &loop, iv, &step_first,
                                  &step_last)) {
      safety_trace("the loop index does not step by one",
                   access->location.line);
      return 0;
    }
    if (safety_symbol_written_between(function, loop.branch_index + 1,
                                      loop.jump_index, iv, step_first,
                                      step_last)) {
      safety_trace("the loop index is assigned inside the body",
                   access->location.line);
      return 0;
    }

    /* `iv < bound` with a unit step from zero tops out at bound - 1. */
    long long highest = (bound - 1) * stride;
    if (highest < 0 || access->size > access->extent) {
      return 0;
    }
    if (highest <= access->extent - access->size) {
      return 1;
    }
    safety_trace("the loop can reach past the end of the object",
                 access->location.line);
    return 0;
  }

  safety_trace("no enclosing loop bounds this index", access->location.line);
  return 0;
}

static int safety_prove(IRFunction *function, size_t check_index,
                        const SafetyAccess *access) {
  return safety_prove_constant(function, check_index, access) ||
         safety_prove_loop_bound(function, check_index, access);
}

/* ---- the two survivor shapes ----------------------------------------------- */

/* The object's size is a compile time constant, so the whole check is one
 * unsigned comparison.
 *
 * Comparing without sign is what lets a single test cover both ends: a
 * negative offset reads as an enormous unsigned value and fails the same
 * comparison an oversized one does. The alternative, a signed `offset <
 * extent`, waves every negative index straight through.
 *
 * The trap arm converts the byte offset back into an element index so the
 * message speaks in the units the programmer wrote. It sits after the branch,
 * so that division costs nothing on the path that stays in bounds. */
static int safety_expand_extent(IRInstructionVector *out,
                                const SafetyAccess *access) {
  char ok_label[64];
  char condition[64];
  char index_temp[64];
  unsigned id = g_safety_next_id++;
  snprintf(ok_label, sizeof(ok_label), SAFETY_LABEL_PREFIX "%u", id);
  snprintf(condition, sizeof(condition), SAFETY_TEMP_PREFIX "c%u", id);
  snprintf(index_temp, sizeof(index_temp), SAFETY_TEMP_PREFIX "i%u", id);

  char message[192];
  snprintf(message, sizeof(message), "Fatal error: `%s` is outside its bounds",
           access->what);

  /* An access wider than the whole object can never fit. Emitting the
   * comparison would underflow the limit into a huge unsigned bound and let it
   * pass, so trap outright. */
  if (access->size > access->extent) {
    IROperand arguments[4];
    arguments[0] = ir_operand_int(2);
    arguments[1] = ir_operand_string(message);
    arguments[2] = ir_operand_int(0);
    arguments[3] = ir_operand_int(0);
    int ok = safety_emit_call(out, access->location, "mettle_crash_trap_ex",
                              arguments, 4);
    for (size_t i = 0; i < 4; i++) {
      ir_operand_destroy(&arguments[i]);
    }
    return ok;
  }

  IROperand limit = ir_operand_int(access->extent - access->size);
  int emitted = safety_emit_binary(out, access->location, ">", condition,
                                   access->offset, &limit, 1) &&
                safety_emit_branch_zero(out, access->location, condition,
                                        ok_label);
  ir_operand_destroy(&limit);
  if (!emitted) {
    return 0;
  }

  IROperand element_size = ir_operand_int(access->size);
  int trapped = safety_emit_binary(out, access->location, "/", index_temp,
                                   access->offset, &element_size, 0);
  ir_operand_destroy(&element_size);
  if (!trapped) {
    return 0;
  }

  IROperand arguments[4];
  arguments[0] = ir_operand_int(2); /* METTLE_CRASH_TRAP_ARRAY_BOUNDS */
  arguments[1] = ir_operand_string(message);
  arguments[2] = ir_operand_temp(index_temp);
  arguments[3] = ir_operand_int(access->extent / access->size);
  int ok = arguments[1].kind == IR_OPERAND_STRING && arguments[2].name &&
           safety_emit_call(out, access->location, "mettle_crash_trap_ex",
                            arguments, 4);
  for (size_t i = 0; i < 4; i++) {
    ir_operand_destroy(&arguments[i]);
  }
  if (!ok) {
    return 0;
  }

  return safety_emit_label(out, access->location, ok_label);
}

/* Only the runtime knows how large the allocation behind this pointer is, so
 * hand it the base, the displacement and the width and let the shadow map
 * answer. The base rather than the final address is what carries provenance:
 * it is the allocation the pointer came from that bounds the access, not
 * whichever one the computed address happens to land in. */
static int safety_expand_region(IRInstructionVector *out,
                                const SafetyAccess *access) {
  IROperand arguments[5];
  size_t built = 0;
  int ok = 0;

  if (!ir_operand_clone(access->base, &arguments[0])) {
    return 0;
  }
  built = 1;
  if (!ir_operand_clone(access->offset, &arguments[1])) {
    goto done;
  }
  built = 2;
  arguments[2] = ir_operand_int(access->size);
  arguments[3] = ir_operand_int(access->access_kind);
  arguments[4] = ir_operand_int((long long)access->location.line);
  built = 5;

  ok = safety_emit_call(out, access->location, "mettle_safety_check", arguments,
                        5);

done:
  for (size_t i = 0; i < built; i++) {
    ir_operand_destroy(&arguments[i]);
  }
  return ok;
}

/* ---- the one exempt module ------------------------------------------------- */

/* An allocator is the one piece of code whose job is to touch memory that is
 * not inside any live allocation. It writes a block header below the pointer
 * it hands out, and it threads its free list through the bodies of blocks the
 * program has already released. Checked against the model those accesses read
 * as a header overrun and a use-after-free, and they are neither: the model is
 * describing the allocator's own bookkeeping as if it were program memory.
 *
 * So the allocator is exempt, identified by role rather than by path: it is
 * whichever source file defines the heap entry points. Nothing else is exempt,
 * and the exemption costs no coverage of the program itself, because the
 * program only reaches this memory through pointers the allocator returned. */
static const char *safety_allocator_source(const IRProgram *program) {
  for (size_t i = 0; i < program->function_count; i++) {
    const IRFunction *function = program->functions[i];
    if (function && function->name &&
        strncmp(function->name, "mettle_heap_", 12) == 0) {
      return function->location.filename;
    }
  }
  return NULL;
}

static int safety_function_is_allocator(const IRFunction *function,
                                        const char *allocator_source) {
  if (!allocator_source || !function) {
    return 0;
  }
  if (function->name && strncmp(function->name, "mettle_heap_", 12) == 0) {
    return 1;
  }
  return function->location.filename &&
         strcmp(function->location.filename, allocator_source) == 0;
}

/* ---- driver ---------------------------------------------------------------- */

/* Drop every check in a function without expanding any of them. */
static int safety_strip_function(IRFunction *function, IRSafetyStats *stats) {
  IRInstructionVector out = {0};
  if (!ir_instruction_vector_reserve(&out, function->instruction_count)) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_SAFETY_CHECK) {
      if (stats) {
        stats->emitted++;
        stats->exempt++;
      }
      continue;
    }
    if (!ir_instruction_vector_append_move(&out, instruction)) {
      ir_instruction_vector_destroy(&out);
      return 0;
    }
  }
  if (!ir_function_replace_instructions(function, &out)) {
    ir_instruction_vector_destroy(&out);
    return 0;
  }
  return 1;
}

static int safety_resolve_function(IRFunction *function, IRSafetyStats *stats) {
  size_t check_count = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_SAFETY_CHECK) {
      check_count++;
    }
  }
  if (check_count == 0) {
    return 1;
  }

  /* Prove first, rewrite second. The proofs read the instructions that produced
   * a check's operands, and rewriting moves instructions out of the array as it
   * goes, so a proof running mid-rewrite would look back at emptied slots and
   * conclude it knows nothing. */
  unsigned char *proved = calloc(function->instruction_count, 1);
  if (!proved) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];
    if (instruction->op != IR_OP_SAFETY_CHECK) {
      continue;
    }
    SafetyAccess access;
    if (!safety_read(instruction, &access)) {
      free(proved);
      return 0;
    }
    proved[i] = safety_prove(function, i, &access) ? 1u : 0u;
  }

  IRInstructionVector out = {0};
  if (!ir_instruction_vector_reserve(&out, function->instruction_count + 16)) {
    free(proved);
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];
    if (instruction->op != IR_OP_SAFETY_CHECK) {
      if (!ir_instruction_vector_append_move(&out, instruction)) {
        ir_instruction_vector_destroy(&out);
        free(proved);
        return 0;
      }
      continue;
    }

    SafetyAccess access;
    if (!safety_read(instruction, &access)) {
      ir_instruction_vector_destroy(&out);
      free(proved);
      return 0;
    }
    if (stats) {
      stats->emitted++;
    }

    if (proved[i]) {
      if (stats) {
        stats->proved++;
      }
      continue;
    }

    int expanded;
    if (access.extent == IR_SAFETY_EXTENT_UNKNOWN) {
      expanded = safety_expand_region(&out, &access);
      if (expanded) {
        if (stats) {
          stats->region_calls++;
        }
        ir_explain_safety_note(access.location.filename, access.location.line,
                               function->name, IR_SAFETY_SURVIVOR_REGION);
      }
    } else {
      expanded = safety_expand_extent(&out, &access);
      if (expanded) {
        if (stats) {
          stats->extent_tests++;
        }
        ir_explain_safety_note(access.location.filename, access.location.line,
                               function->name, IR_SAFETY_SURVIVOR_EXTENT);
      }
    }
    if (!expanded) {
      ir_instruction_vector_destroy(&out);
      free(proved);
      return 0;
    }
    /* The check is not moved into `out`: its operands were cloned into the
     * replacement, and ir_function_replace_instructions frees what is left of
     * the old array below. */
  }

  free(proved);
  if (!ir_function_replace_instructions(function, &out)) {
    ir_instruction_vector_destroy(&out);
    return 0;
  }
  return 1;
}

int ir_safety_resolve_program(IRProgram *program, IRSafetyStats *stats) {
  if (!program) {
    return 1;
  }
  g_safety_next_id = 0;
  const char *allocator_source = safety_allocator_source(program);
  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *function = program->functions[i];
    if (!function) {
      continue;
    }
    int resolved =
        safety_function_is_allocator(function, allocator_source)
            ? safety_strip_function(function, stats)
            : safety_resolve_function(function, stats);
    if (!resolved) {
      return 0;
    }
  }
  return 1;
}

/* ---- telling the runtime where the heap is --------------------------------- */

typedef enum {
  SAFETY_ALLOC_NONE = 0,
  SAFETY_ALLOC_SIZE,    /* arguments[0] is the byte count */
  SAFETY_ALLOC_PRODUCT, /* arguments[0] * arguments[1] is the byte count */
  SAFETY_ALLOC_REALLOC, /* arguments[0] the old block, arguments[1] the size */
  SAFETY_ALLOC_FREE     /* arguments[0] is the block being retired */
} SafetyAllocKind;

/* Whether the callee is the Mettle-implemented allocator rather than the libc
 * one. Only the former needs bracketing: its body is Mettle code that lowering
 * has checked, and it reaches for the same helpers ordinary code does. The
 * libc allocator is C, never carries a check, and needs no bracket. */
static int safety_callee_is_mettle_allocator(const IRInstruction *instruction) {
  return instruction->op == IR_OP_CALL && instruction->text &&
         strncmp(instruction->text, "mettle_heap_", 12) == 0;
}

/* Both spellings of every entry point: the libc names a program calls by
 * default, and the std/alloc names --native-heap rewrites them to. This runs
 * after that rewrite, so only one set is ever present, but matching both keeps
 * the two flags independent. */
static SafetyAllocKind safety_classify_call(const IRInstruction *instruction) {
  if (instruction->op != IR_OP_CALL || !instruction->text) {
    return SAFETY_ALLOC_NONE;
  }
  const char *callee = instruction->text;
  size_t arguments = instruction->argument_count;

  if (arguments == 1 &&
      (strcmp(callee, "malloc") == 0 ||
       strcmp(callee, "mettle_heap_alloc") == 0 ||
       strcmp(callee, "mettle_heap_zeroed") == 0)) {
    return SAFETY_ALLOC_SIZE;
  }
  if (arguments == 2 && (strcmp(callee, "calloc") == 0 ||
                         strcmp(callee, "mettle_heap_calloc") == 0)) {
    return SAFETY_ALLOC_PRODUCT;
  }
  if (arguments == 2 && (strcmp(callee, "realloc") == 0 ||
                         strcmp(callee, "mettle_heap_realloc") == 0)) {
    return SAFETY_ALLOC_REALLOC;
  }
  if (arguments == 1 && (strcmp(callee, "free") == 0 ||
                         strcmp(callee, "mettle_heap_free") == 0)) {
    return SAFETY_ALLOC_FREE;
  }
  return SAFETY_ALLOC_NONE;
}

static int safety_emit_register(IRInstructionVector *out,
                                SourceLocation location,
                                const IROperand *pointer,
                                const IROperand *size) {
  IROperand arguments[2];
  if (!ir_operand_clone(pointer, &arguments[0])) {
    return 0;
  }
  if (!ir_operand_clone(size, &arguments[1])) {
    ir_operand_destroy(&arguments[0]);
    return 0;
  }
  int ok = safety_emit_call(out, location, "mettle_safety_register", arguments,
                            2);
  ir_operand_destroy(&arguments[0]);
  ir_operand_destroy(&arguments[1]);
  return ok;
}

static int safety_emit_one_pointer_call(IRInstructionVector *out,
                                        SourceLocation location,
                                        const char *callee,
                                        const IROperand *pointer) {
  IROperand argument;
  if (!ir_operand_clone(pointer, &argument)) {
    return 0;
  }
  int ok = safety_emit_call(out, location, callee, &argument, 1);
  ir_operand_destroy(&argument);
  return ok;
}

static int safety_emit_reregister(IRInstructionVector *out,
                                  SourceLocation location,
                                  const IROperand *old_pointer,
                                  const IROperand *new_pointer,
                                  const IROperand *size) {
  IROperand arguments[3];
  size_t built = 0;
  int ok = 0;

  if (!ir_operand_clone(old_pointer, &arguments[0])) {
    return 0;
  }
  built = 1;
  if (!ir_operand_clone(new_pointer, &arguments[1])) {
    goto done;
  }
  built = 2;
  if (!ir_operand_clone(size, &arguments[2])) {
    goto done;
  }
  built = 3;
  ok = safety_emit_call(out, location, "mettle_safety_reregister", arguments,
                        3);

done:
  for (size_t i = 0; i < built; i++) {
    ir_operand_destroy(&arguments[i]);
  }
  return ok;
}

/* The size `new T` asks for. Mirrors what --native-heap's rewrite does with
 * the same operand, including its eight byte fallback for a missing one. */
static IROperand safety_new_size(const IRInstruction *instruction) {
  if (instruction->rhs.kind == IR_OPERAND_NONE ||
      (instruction->rhs.kind == IR_OPERAND_INT &&
       instruction->rhs.int_value <= 0)) {
    return ir_operand_int(8);
  }
  return instruction->rhs;
}

static int safety_register_function(IRFunction *function) {
  int found = 0;
  for (size_t i = 0; i < function->instruction_count && !found; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    found = instruction->op == IR_OP_NEW ||
            safety_classify_call(instruction) != SAFETY_ALLOC_NONE;
  }
  if (!found) {
    return 1;
  }

  IRInstructionVector out = {0};
  if (!ir_instruction_vector_reserve(&out, function->instruction_count + 16)) {
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];
    SafetyAllocKind kind = safety_classify_call(instruction);
    int is_new = instruction->op == IR_OP_NEW;
    SourceLocation location = instruction->location;

    if (kind == SAFETY_ALLOC_NONE && !is_new) {
      if (!ir_instruction_vector_append_move(&out, instruction)) {
        ir_instruction_vector_destroy(&out);
        return 0;
      }
      continue;
    }

    /* Retire the block before the call that releases it, not after. Between
     * the two the allocator has not handed the memory out yet, so no other
     * thread can register something else over it. */
    if (kind == SAFETY_ALLOC_FREE) {
      if (!safety_emit_one_pointer_call(&out, location,
                                        "mettle_safety_unregister",
                                        &instruction->arguments[0])) {
        ir_instruction_vector_destroy(&out);
        return 0;
      }
    }

    /* How many bytes the call is about to hand back. calloc states it as a
     * product, which is multiplied out here, before the call, so only the one
     * result has its live range stretched across it instead of both factors. */
    IROperand size = ir_operand_none();
    IROperand product_operand = ir_operand_none();
    if (is_new) {
      size = safety_new_size(instruction);
    } else if (kind == SAFETY_ALLOC_SIZE) {
      size = instruction->arguments[0];
    } else if (kind == SAFETY_ALLOC_REALLOC) {
      size = instruction->arguments[1];
    } else if (kind == SAFETY_ALLOC_PRODUCT) {
      char product[64];
      snprintf(product, sizeof(product), SAFETY_TEMP_PREFIX "n%u",
               g_safety_next_id++);
      product_operand = ir_operand_temp(product);
      if (!product_operand.name ||
          !safety_emit_binary(&out, location, "*", product,
                              &instruction->arguments[0],
                              &instruction->arguments[1], 1)) {
        ir_operand_destroy(&product_operand);
        ir_instruction_vector_destroy(&out);
        return 0;
      }
      size = product_operand;
    }

    /* These alias the instruction's own storage, which the vector takes over
     * below and keeps alive for the rest of this function. */
    IROperand result = instruction->dest;
    IROperand old_pointer = kind == SAFETY_ALLOC_REALLOC
                                ? instruction->arguments[0]
                                : ir_operand_none();
    int bracket = safety_callee_is_mettle_allocator(instruction);

    if (bracket && !safety_emit_call(&out, location,
                                     "mettle_safety_enter_allocator", NULL,
                                     0)) {
      ir_operand_destroy(&product_operand);
      ir_instruction_vector_destroy(&out);
      return 0;
    }

    if (!ir_instruction_vector_append_move(&out, instruction)) {
      ir_operand_destroy(&product_operand);
      ir_instruction_vector_destroy(&out);
      return 0;
    }

    if (bracket && !safety_emit_call(&out, location,
                                     "mettle_safety_leave_allocator", NULL,
                                     0)) {
      ir_operand_destroy(&product_operand);
      ir_instruction_vector_destroy(&out);
      return 0;
    }

    /* A result nobody keeps cannot be reached through, so there is nothing to
     * describe. Freeing has already been handled above. */
    int ok = 1;
    if (result.kind != IR_OPERAND_NONE && kind != SAFETY_ALLOC_FREE) {
      ok = kind == SAFETY_ALLOC_REALLOC
               ? safety_emit_reregister(&out, location, &old_pointer, &result,
                                        &size)
               : safety_emit_register(&out, location, &result, &size);
    }
    ir_operand_destroy(&product_operand);
    if (!ok) {
      ir_instruction_vector_destroy(&out);
      return 0;
    }
  }

  if (!ir_function_replace_instructions(function, &out)) {
    ir_instruction_vector_destroy(&out);
    return 0;
  }
  ir_function_clear_cfg(function);
  return 1;
}

int ir_safety_register_allocations(IRProgram *program) {
  if (!program) {
    return 1;
  }
  const char *allocator_source = safety_allocator_source(program);
  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *function = program->functions[i];
    if (!function) {
      continue;
    }
    /* Exempt for the same reason its accesses are: the calls it makes to
     * itself are the allocator working, not the program allocating, and
     * describing them would register a block once per layer. */
    if (safety_function_is_allocator(function, allocator_source)) {
      continue;
    }
    if (!safety_register_function(function)) {
      return 0;
    }
  }
  return 1;
}
