/* `--safe`: resolving the access marks lowering left behind.
 *
 * Every IR_OP_SAFETY_CHECK is either deleted, because the access provably
 * cannot leave its object, or rewritten into ordinary IR. Nothing downstream
 * sees the opcode. See ir_safety.h for why this runs before the optimizer. */

#include "ir_optimize_internal.h"
#include "../ir_explain_safety.h"
#include "../ir_safety.h"
#include <time.h>

/* Distinct from lowering's ".t%d" temps and from every label prefix the
 * recognizers match on, so a resolved check can never be mistaken for one. */
#define SAFETY_TEMP_PREFIX ".safe"
#define SAFETY_LABEL_PREFIX "ir_safe_ok_"

static unsigned g_safety_next_id;

static int safety_env_flag(const char *name, int *cache) {
  if (*cache < 0) {
    const char *value = getenv(name);
    *cache = value && value[0] && value[0] != '0';
  }
  return *cache;
}

/* METTLE_SAFETY_TRACE=1 prints why a proof gave up. A check that survives is
 * either a real limit of the analysis or a shape it should have recognized,
 * and from the outside those look identical: both are just a check that is
 * still there. Same purpose as the backend's mir_call_trace. */
static int safety_trace_enabled(void) {
  static int state = -1;
  return safety_env_flag("METTLE_SAFETY_TRACE", &state);
}

/* METTLE_SAFETY_TIME=1 reports how long resolving took. Separate from the
 * trace because that prints a line per unproven access, which on a large input
 * costs far more than the work being measured. */
static int safety_time_enabled(void) {
  static int state = -1;
  return safety_env_flag("METTLE_SAFETY_TIME", &state);
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

/* A loop, and what it does to its index when that can be read off the header.
 *
 * The two halves are separate because they answer different questions. Where a
 * loop starts and ends is enough to resolve a pointer once and compare against
 * it, and every loop has that. What its index does is needed to argue about
 * which elements it reaches, and plenty of loops do not say: `while (child <=
 * end)` in a sift-down steps nothing this can read. Refusing to record such a
 * loop at all, which is what the first version did, denied the cheap
 * transformation to exactly the code that needed it most. */
typedef struct {
  size_t header_index;
  IRWhileLoopBounds bounds;
  int has_index;        /* the fields below mean anything */
  const char *iv;
  long long step;       /* constant, greater than zero */
  long long adjust;     /* highest index reached is `bound + adjust` */
  const IROperand *bound;
  size_t step_first;
  size_t step_last;
} SafetyLoopForm;

/* Every loop in the function, in source order, so scanning it backwards finds
 * the innermost one containing a given instruction first.
 *
 * Built once per function because it used to be rebuilt per check: each one
 * scanned backwards over every preceding instruction hunting for a header, and
 * parsed each candidate forwards. On one function holding eight thousand
 * accesses that cost several seconds on its own, and grew faster than the
 * input did. */
typedef struct {
  SafetyLoopForm *items;
  size_t count;
  size_t capacity;
} SafetyLoopList;

/* Innermost loop whose body holds `index`, or NULL. */
static const SafetyLoopForm *safety_enclosing_loop(const SafetyLoopList *loops,
                                                  size_t index);

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

/* Read an index as `variable + constant`, which is how `a[i]` and `a[i + 2]`
 * both arrive. A bare variable is the same thing with a zero constant. */
static int safety_index_is_affine(const IRFunction *function, size_t before,
                                  const IROperand *index, const char **name_out,
                                  long long *addend_out) {
  if (index->kind == IR_OPERAND_SYMBOL && index->name) {
    *name_out = index->name;
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
    return safety_index_is_affine(function, before, &producer->lhs, name_out,
                                  addend_out);
  }
  if (producer->op != IR_OP_BINARY || !producer->text ||
      producer->lhs.kind != IR_OPERAND_SYMBOL || !producer->lhs.name ||
      producer->rhs.kind != IR_OPERAND_INT) {
    return 0;
  }
  if (strcmp(producer->text, "+") == 0) {
    *name_out = producer->lhs.name;
    *addend_out = producer->rhs.int_value;
    return 1;
  }
  if (strcmp(producer->text, "-") == 0) {
    *name_out = producer->lhs.name;
    *addend_out = -producer->rhs.int_value;
    return 1;
  }
  return 0;
}

/* The largest value an index can take, when that follows from the arithmetic
 * alone rather than from any loop.
 *
 * Masking is the case worth reading: `alpha[(bits >> 2) & 63]` cannot leave
 * [0, 63] whatever `bits` holds, because a non-negative mask clears every
 * higher bit including the sign. That is the shape of every table lookup, and
 * it bounds the access without knowing anything about the surrounding code. */
static int safety_index_upper_bound(const IRFunction *function, size_t before,
                                    const IROperand *index, int depth,
                                    long long *upper_out) {
  if (index->kind == IR_OPERAND_INT) {
    if (index->int_value < 0) {
      return 0;
    }
    *upper_out = index->int_value;
    return 1;
  }
  if (index->kind != IR_OPERAND_TEMP || !index->name || depth > 4) {
    return 0;
  }
  const IRInstruction *producer =
      ir_find_temp_producer_before(function, before, index->name);
  if (!producer || producer->is_float) {
    return 0;
  }
  if (producer->op == IR_OP_ASSIGN) {
    return safety_index_upper_bound(function, before, &producer->lhs, depth + 1,
                                    upper_out);
  }
  if (producer->op != IR_OP_BINARY || !producer->text ||
      strcmp(producer->text, "&") != 0) {
    return 0;
  }
  if (producer->rhs.kind == IR_OPERAND_INT && producer->rhs.int_value >= 0) {
    *upper_out = producer->rhs.int_value;
    return 1;
  }
  if (producer->lhs.kind == IR_OPERAND_INT && producer->lhs.int_value >= 0) {
    *upper_out = producer->lhs.int_value;
    return 1;
  }
  return 0;
}

/* Read the index out of the multiply lowering emits for a subscript, and
 * report the largest value it can take. */
static int safety_offset_upper_bound(const IRFunction *function,
                                     size_t check_index,
                                     const IROperand *offset,
                                     long long *stride_out,
                                     long long *upper_out) {
  if (offset->kind != IR_OPERAND_TEMP || !offset->name) {
    return 0;
  }
  const IRInstruction *producer =
      ir_find_temp_producer_before(function, check_index, offset->name);
  if (!producer || producer->op != IR_OP_BINARY || producer->is_float ||
      !producer->text || strcmp(producer->text, "*") != 0 ||
      producer->rhs.kind != IR_OPERAND_INT || producer->rhs.int_value <= 0) {
    return 0;
  }
  size_t producer_index = (size_t)(producer - function->instructions);
  if (!safety_index_upper_bound(function, producer_index, &producer->lhs, 0,
                                upper_out)) {
    return 0;
  }
  *stride_out = producer->rhs.int_value;
  return 1;
}

/* Read `(iv + addend) * stride` out of the instruction that produced the
 * offset. This is the shape lowering emits for every subscript: the index,
 * then a multiply by the element width. */
static int safety_offset_is_scaled_symbol(const IRFunction *function,
                                          size_t check_index,
                                          const IROperand *offset,
                                          const char **iv_out,
                                          long long *stride_out,
                                          long long *addend_out) {
  if (offset->kind != IR_OPERAND_TEMP || !offset->name) {
    return 0;
  }
  const IRInstruction *producer =
      ir_find_temp_producer_before(function, check_index, offset->name);
  if (!producer || producer->op != IR_OP_BINARY || producer->is_float ||
      !producer->text || strcmp(producer->text, "*") != 0 ||
      producer->rhs.kind != IR_OPERAND_INT || producer->rhs.int_value <= 0) {
    return 0;
  }
  size_t producer_index = (size_t)(producer - function->instructions);
  if (!safety_index_is_affine(function, producer_index, &producer->lhs, iv_out,
                              addend_out)) {
    return 0;
  }
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

/* `<anything> = iv + <positive constant>`, the arithmetic half of a step. */
static int safety_read_step_add(const IRInstruction *instruction,
                                const char *iv, long long *step_out) {
  if (!instruction || instruction->op != IR_OP_BINARY ||
      instruction->is_float || !instruction->text ||
      strcmp(instruction->text, "+") != 0 ||
      !ir_operand_is_symbol_named(&instruction->lhs, iv) ||
      instruction->rhs.kind != IR_OPERAND_INT ||
      instruction->rhs.int_value <= 0) {
    return 0;
  }
  *step_out = instruction->rhs.int_value;
  return 1;
}

/* Read how far the loop advances its index each iteration, and report which
 * instructions do it.
 *
 * The body is searched rather than just its last instruction, because a loop
 * often advances more than one counter and only one of them is the index this
 * access uses. Exactly one write to it is required, which is also what proves
 * nothing else in the body moves it.
 *
 * Two shapes, because this pass runs before the optimizer: lowering emits the
 * step as a pair, `t = i + 3` followed by `i = t`, and copy propagation folds
 * that into the single `i = i + 3` every recognizer downstream expects. Both
 * mean the same thing and both have to be read here. */
static int safety_loop_step(const IRFunction *function,
                            const IRWhileLoopBounds *loop, const char *iv,
                            long long *step_out, size_t *step_first,
                            size_t *step_last) {
  size_t write_index = 0;
  size_t write_count = 0;
  for (size_t i = loop->branch_index + 1;
       i < loop->jump_index && i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->dest.kind == IR_OPERAND_SYMBOL &&
        instruction->dest.name && strcmp(instruction->dest.name, iv) == 0) {
      write_index = i;
      write_count++;
    }
  }
  if (write_count != 1) {
    return 0;
  }

  const IRInstruction *write = &function->instructions[write_index];
  if (safety_read_step_add(write, iv, step_out)) {
    *step_first = write_index;
    *step_last = write_index;
    return 1;
  }

  if (write->op != IR_OP_ASSIGN || write->lhs.kind != IR_OPERAND_TEMP ||
      !write->lhs.name) {
    return 0;
  }
  const IRInstruction *add =
      ir_find_temp_producer_before(function, write_index, write->lhs.name);
  if (!safety_read_step_add(add, iv, step_out)) {
    return 0;
  }
  size_t add_index = (size_t)(add - function->instructions);
  if (add_index <= loop->branch_index || add_index >= loop->jump_index) {
    return 0; /* the arithmetic is not in this body, so it is not the step */
  }
  *step_first = add_index;
  *step_last = write_index;
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
static int safety_prove_loop_bound(IRFunction *function,
                                   const SafetyLoopList *loops,
                                   size_t check_index,
                                   const SafetyAccess *access) {
  if (access->extent == IR_SAFETY_EXTENT_UNKNOWN || access->size <= 0) {
    return 0;
  }

  const char *iv = NULL;
  long long stride = 0;
  long long addend = 0;
  if (!safety_offset_is_scaled_symbol(function, check_index, access->offset,
                                      &iv, &stride, &addend)) {
    safety_trace("the offset is not an index scaled by a constant width",
                 access->location.line);
    return 0;
  }
  if (addend < 0) {
    return 0; /* the loop's lower bound says nothing about a negative offset */
  }

  /* Innermost enclosing loop stepping this variable. Working outward matters:
   * a check in a nested loop is indexed by the inner variable, and the outer
   * loop's bound says nothing about it. */
  for (size_t i = loops->count; i-- > 0;) {
    const SafetyLoopForm *loop = &loops->items[i];
    if (check_index <= loop->bounds.branch_index ||
        check_index >= loop->bounds.jump_index) {
      continue; /* the check is not in this loop's body */
    }
    if (!loop->has_index) {
      continue; /* this loop's test says nothing about any index */
    }
    if (strcmp(loop->iv, iv) != 0) {
      continue; /* this loop steps a different variable; keep looking outward */
    }

    long long bound = 0;
    if (!safety_constant_value(function, loop->bounds.compare_index,
                               loop->bound, 0, &bound)) {
      safety_trace("the loop bound is not a constant", access->location.line);
      return 0;
    }
    /* `iv <op> bound` becomes `iv <= bound + adjust`, whichever way the test
     * was spelled. */
    long long highest_index = bound + loop->adjust;
    if (highest_index < 0) {
      return 1; /* the body never runs, so the access never happens */
    }

    if (!ir_iv_zero_at_header(function, loop->header_index, iv)) {
      safety_trace("the loop index does not start at zero",
                   access->location.line);
      return 0;
    }
    size_t step_first = 0;
    size_t step_last = 0;
    long long step = 0;
    if (!safety_loop_step(function, &loop->bounds, iv, &step, &step_first,
                          &step_last)) {
      safety_trace("the loop index does not step by a constant",
                   access->location.line);
      return 0;
    }
    if (safety_symbol_written_between(function, loop->bounds.branch_index + 1,
                                      loop->bounds.jump_index, iv, step_first,
                                      step_last)) {
      safety_trace("the loop index is assigned inside the body",
                   access->location.line);
      return 0;
    }

    long long highest = (highest_index + addend) * stride;
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

/* The index is masked into a range the object already covers. */
static int safety_prove_masked_index(const IRFunction *function,
                                     size_t check_index,
                                     const SafetyAccess *access) {
  if (access->extent == IR_SAFETY_EXTENT_UNKNOWN || access->size <= 0) {
    return 0;
  }
  long long stride = 0;
  long long upper = 0;
  if (!safety_offset_upper_bound(function, check_index, access->offset, &stride,
                                 &upper)) {
    return 0;
  }
  long long highest = upper * stride;
  if (highest < 0 || access->size > access->extent) {
    return 0;
  }
  return highest <= access->extent - access->size;
}

static int safety_prove(IRFunction *function, const SafetyLoopList *loops,
                        size_t check_index, const SafetyAccess *access) {
  return safety_prove_constant(function, check_index, access) ||
         safety_prove_loop_bound(function, loops, check_index, access) ||
         safety_prove_masked_index(function, check_index, access);
}

/* ---- hoisting a loop's checks into one ------------------------------------- */
/*
 * When the object's size is not known, a check per access means a call per
 * access, and in a tight loop that is the whole cost of the mode. But a
 * counted loop walking `base[i]` for i in [0, bound) touches one contiguous
 * range, and one check covers the lot. So the check moves out of the loop and
 * becomes a statement about the range, and the body is left with nothing in
 * it, which is also what lets the vectorizers claim it again.
 *
 * Correctness rests on the range being exactly what the loop touches, no more
 * and no less. More would trap on a correct program; less would miss a real
 * overrun. That is why the body has to be straight line (a conditional access
 * touches a subset, so checking the whole range could accuse a program that
 * never reads the far end) and why it must contain no calls (one of them could
 * free the block partway through, which a check taken beforehand would miss).
 */

typedef struct {
  size_t header_index; /* the loop label the check moves in front of */
  /* When the reach of the access follows from the arithmetic alone, as a
   * masked index does, the range is this many bytes and none of the loop
   * fields below are read. */
  long long constant_length;
  long long stride;       /* bytes per element */
  long long primary_step; /* how far the tested variable moves each iteration */
  long long index_step;   /* how far the indexing variable moves */
  long long adjust;       /* the tested variable tops out at bound + adjust */
  long long addend;       /* the index is that variable plus this */
  long long size;         /* bytes the access touches */
  long long access_kind;
  IROperand base;  /* owned */
  IROperand bound; /* owned */
  SourceLocation location;
} SafetyHoist;


/* Read the loop's test and step.
 *
 * The test is `index <op> bound` for `<` or `<=`, where the index may carry a
 * constant of its own: `while (i + 3 <= len)` is how a loop consuming three
 * bytes at a time says where it stops. Each spelling gives a different highest
 * index, and getting that wrong by one is the difference between checking what
 * the loop touches and checking a byte past it. */
static int safety_parse_loop_form(const IRFunction *function,
                                  size_t header_index, SafetyLoopForm *form) {
  if (header_index + 4 >= function->instruction_count) {
    return 0;
  }
  const IRInstruction *header = &function->instructions[header_index];
  if (header->op != IR_OP_LABEL || !ir_label_is_while_header(header->text)) {
    return 0;
  }

  /* Find the exit test, then work back to what computed it. A test that needs
   * arithmetic of its own, as `i + 3 <= len` does, puts that arithmetic
   * between the header and the compare, so counting instructions forward from
   * the header finds the wrong one. */
  size_t branch_index = 0;
  int found_branch = 0;
  for (size_t i = header_index + 1; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_BRANCH_ZERO) {
      branch_index = i;
      found_branch = 1;
      break;
    }
    if (instruction->op == IR_OP_LABEL || instruction->op == IR_OP_JUMP ||
        instruction->op == IR_OP_BRANCH_EQ) {
      return 0;
    }
  }
  if (!found_branch) {
    return 0;
  }

  const IRInstruction *branch = &function->instructions[branch_index];
  if (!branch->text || branch->lhs.kind != IR_OPERAND_TEMP ||
      !branch->lhs.name) {
    return 0;
  }
  const IRInstruction *compare =
      ir_find_temp_producer_before(function, branch_index, branch->lhs.name);
  if (!compare || compare->op != IR_OP_BINARY || compare->is_float ||
      !compare->text) {
    return 0;
  }
  size_t compare_index = (size_t)(compare - function->instructions);
  if (compare_index <= header_index) {
    return 0;
  }

  form->header_index = header_index;
  form->bounds.compare_index = compare_index;
  form->bounds.branch_index = branch_index;
  form->bounds.loop_label = header->text;
  form->bounds.exit_label = branch->text;
  form->bounds.jump_index = (size_t)-1;
  for (size_t i = branch_index + 1; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_JUMP && instruction->text &&
        strcmp(instruction->text, form->bounds.loop_label) == 0) {
      form->bounds.jump_index = i;
      break;
    }
    if (instruction->op == IR_OP_LABEL && instruction->text &&
        strcmp(instruction->text, form->bounds.exit_label) == 0) {
      break;
    }
  }
  if (form->bounds.jump_index == (size_t)-1) {
    return 0;
  }

  /* Where the loop runs is settled. Whether its test also says what the index
   * does is a separate question, and a loop that does not say is still a loop
   * worth knowing about. */
  form->has_index = 0;
  long long index_addend = 0;
  if (safety_index_is_affine(function, compare_index, &compare->lhs, &form->iv,
                             &index_addend)) {
    if (strcmp(compare->text, "<") == 0) {
      form->adjust = -index_addend - 1;
      form->has_index = 1;
    } else if (strcmp(compare->text, "<=") == 0) {
      form->adjust = -index_addend;
      form->has_index = 1;
    }
  }
  form->bound = &compare->rhs;
  return 1;
}

/* An operand whose value cannot change across [start, end). */
static int safety_operand_invariant_in(const IRFunction *function, size_t start,
                                       size_t end, const IROperand *operand) {
  if (operand->kind == IR_OPERAND_INT) {
    return 1;
  }
  if ((operand->kind != IR_OPERAND_SYMBOL && operand->kind != IR_OPERAND_TEMP) ||
      !operand->name) {
    return 0;
  }
  for (size_t i = start; i < end && i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->dest.name &&
        strcmp(instruction->dest.name, operand->name) == 0) {
      return 0;
    }
  }
  return 1;
}

/* Nothing in the body can release the memory the loop is walking. Weaker than
 * requiring a straight line, deliberately: to reuse one resolved allocation
 * across many accesses it only matters that the allocation outlives them, not
 * that every access happens. Branches are fine, because each access still
 * carries its own comparison. */
static int safety_body_has_no_calls(const IRFunction *function,
                                    const IRWhileLoopBounds *loop) {
  for (size_t i = loop->branch_index + 1;
       i < loop->jump_index && i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op == IR_OP_NEW || instruction->op == IR_OP_GPU_LAUNCH ||
        instruction->op == IR_OP_CALL_INDIRECT) {
      return 0;
    }
    if (instruction->op == IR_OP_CALL) {
      /* The checks themselves are about to be rewritten, so they do not count
       * against the body; anything else could free what the loop is walking. */
      if (!instruction->text ||
          strncmp(instruction->text, "mettle_safety_", 14) != 0) {
        return 0;
      }
    }
  }
  return 1;
}

/* Every iteration runs every instruction, and none of them can release the
 * memory being walked. A jump back to the header is the loop closing and is
 * expected; anything else branching is not. */
static int safety_body_is_straight_line(const IRFunction *function,
                                        const IRWhileLoopBounds *loop) {
  for (size_t i = loop->branch_index + 1; i < loop->jump_index; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    switch (instruction->op) {
    case IR_OP_LABEL:
    case IR_OP_JUMP:
    case IR_OP_BRANCH_ZERO:
    case IR_OP_BRANCH_EQ:
    case IR_OP_RETURN:
      return 0;
    case IR_OP_CALL:
    case IR_OP_CALL_INDIRECT:
    case IR_OP_NEW:
    case IR_OP_GPU_LAUNCH:
      /* The checks themselves are about to be removed, so they do not count
       * against the body; anything else could free what the loop is walking. */
      if (instruction->op == IR_OP_CALL && instruction->text &&
          strncmp(instruction->text, "mettle_safety_", 14) == 0) {
        continue;
      }
      return 0;
    default:
      continue;
    }
  }
  return 1;
}

static int safety_try_hoist(IRFunction *function, const SafetyLoopList *loops,
                            size_t check_index, const SafetyAccess *access,
                            SafetyHoist *out) {
  if (access->extent != IR_SAFETY_EXTENT_UNKNOWN || access->size <= 0) {
    return 0;
  }

  /* An index the arithmetic already bounds, such as a masked table lookup,
   * reaches the same range on every iteration. One check for that range stands
   * in for all of them, and its length is a constant. */
  long long masked_stride = 0;
  long long masked_upper = 0;
  int masked = safety_offset_upper_bound(function, check_index, access->offset,
                                         &masked_stride, &masked_upper);

  const char *iv = NULL;
  long long stride = 0;
  long long addend = 0;
  if (!masked &&
      !safety_offset_is_scaled_symbol(function, check_index, access->offset,
                                      &iv, &stride, &addend)) {
    safety_trace("the index is neither a counter nor bounded by its own "
                 "arithmetic",
                 access->location.line);
    return 0;
  }
  /* An access no wider than its element is what keeps the hoisted length from
   * reaching past the range the expression describes. */
  if (!masked && (access->size > stride || addend < 0)) {
    safety_trace("the access is wider than its element, or reaches backwards",
                 access->location.line);
    return 0;
  }

  if (masked) {
    const SafetyLoopForm *innermost = safety_enclosing_loop(loops, check_index);
    if (!innermost) {
      safety_trace("the index is bounded but there is no loop to lift the "
                   "check out of",
                   access->location.line);
      return 0;
    }
    if (!safety_body_is_straight_line(function, &innermost->bounds) ||
        !safety_operand_invariant_in(function, innermost->header_index,
                                     innermost->bounds.jump_index,
                                     access->base)) {
      return 0;
    }
    out->header_index = innermost->header_index;
    out->constant_length = masked_upper * masked_stride + access->size;
    out->access_kind = access->access_kind;
    out->location = access->location;
    return ir_operand_clone(access->base, &out->base);
  }

  int saw_loop = 0;
  for (size_t loop_index = loops->count; loop_index-- > 0;) {
    SafetyLoopForm form = loops->items[loop_index];
    size_t header = form.header_index;
    if (check_index <= form.bounds.branch_index ||
        check_index >= form.bounds.jump_index || !form.has_index) {
      continue;
    }
    saw_loop = 1;

    /* The variable the loop tests, and the one this access indexes by, need
     * not be the same. A loop reading three bytes and writing four advances
     * two counters; the test bounds one of them, and the other is pinned to it
     * by both starting at zero and both stepping by a constant, so after the
     * same number of iterations each is that count times its own step. */
    size_t primary_first = 0;
    size_t primary_last = 0;
    if (!ir_iv_zero_at_header(function, header, form.iv) ||
        !safety_loop_step(function, &form.bounds, form.iv, &form.step,
                          &primary_first, &primary_last)) {
      safety_trace("the tested variable is not a plain zero-based counter",
                   access->location.line);
      return 0;
    }

    long long index_step = form.step;
    if (strcmp(form.iv, iv) != 0) {
      size_t index_first = 0;
      size_t index_last = 0;
      if (!ir_iv_zero_at_header(function, header, iv) ||
          !safety_loop_step(function, &form.bounds, iv, &index_step,
                            &index_first, &index_last)) {
        safety_trace("the indexing variable is not a plain zero-based counter",
                     access->location.line);
        return 0;
      }
    }

    if (!safety_body_is_straight_line(function, &form.bounds)) {
      safety_trace("the loop body branches or calls, so one check for the "
                   "whole range would not describe what it touches",
                   access->location.line);
      return 0;
    }
    /* The hoisted check is emitted in front of the header, so both the pointer
     * and the bound have to be settled by then. Scanning from the header
     * rather than from the body is what makes that true of the bound: a test
     * like `while (i < rows * cols)` computes it between the header and the
     * compare, and a check placed in front of the header would name a value
     * that does not exist yet. */
    if (!safety_operand_invariant_in(function, header, form.bounds.jump_index,
                                     access->base) ||
        !safety_operand_invariant_in(function, header, form.bounds.jump_index,
                                     form.bound)) {
      safety_trace("the pointer or the loop bound is not settled before the "
                   "loop starts",
                   access->location.line);
      return 0;
    }

    out->header_index = header;
    out->stride = stride;
    out->primary_step = form.step;
    out->index_step = index_step;
    out->adjust = form.adjust;
    out->addend = addend;
    out->size = access->size;
    out->access_kind = access->access_kind;
    out->location = access->location;
    if (!ir_operand_clone(access->base, &out->base)) {
      return 0;
    }
    if (!ir_operand_clone(form.bound, &out->bound)) {
      ir_operand_destroy(&out->base);
      return 0;
    }
    return 1;
  }
  safety_trace(saw_loop ? "the enclosing loop does not step this index"
                        : "no enclosing loop this pass can read",
               access->location.line);
  return 0;
}

/* Emit the one check that stands in for all of the loop's:
 *
 *   top     = bound + adjust            highest index the loop reaches
 *   settled = (top / step) * step       the last value it actually takes
 *   length  = settled * stride + size   one past the last byte it reads
 *   runs    = top >= 0                  zero when the loop never runs at all
 *   check(base, 0, length * runs)
 *
 * Rounding down to a multiple of the step matters once the step is more than
 * one: a loop counting by three stops at the largest multiple of three below
 * its bound, and using the bound itself would check up to two bytes the loop
 * never reads. On an exactly sized buffer those two bytes are the difference
 * between silence and accusing a correct program. The division is skipped
 * where the step is one, which is most loops.
 *
 * Multiplying by `runs` rather than branching around the check is what keeps
 * this free: a label immediately before a loop header stops the recognizers'
 * backward scan for the induction variable's initial value, so a guard branch
 * here would cost the loop its vectorization, which is most of what hoisting
 * was for. */
static int safety_emit_hoisted(IRInstructionVector *out,
                               const SafetyHoist *hoist) {
  /* A range the arithmetic already settled needs no arithmetic of its own. */
  if (hoist->constant_length > 0) {
    IROperand arguments[5];
    if (!ir_operand_clone(&hoist->base, &arguments[0])) {
      return 0;
    }
    arguments[1] = ir_operand_int(0);
    arguments[2] = ir_operand_int(hoist->constant_length);
    arguments[3] = ir_operand_int(hoist->access_kind);
    arguments[4] = ir_operand_int((long long)hoist->location.line);
    int emitted = safety_emit_call(out, hoist->location, "mettle_safety_check",
                                   arguments, 5);
    ir_operand_destroy(&arguments[0]);
    return emitted;
  }

  enum { SAFETY_HOIST_TEMPS = 7 };
  unsigned id = g_safety_next_id++;
  static const char *const tags[SAFETY_HOIST_TEMPS] = {"ht", "hq", "hm",
                                                       "hi", "hs", "hn", "hr"};
  char names[SAFETY_HOIST_TEMPS][64];
  IROperand temps[SAFETY_HOIST_TEMPS];
  for (int t = 0; t < SAFETY_HOIST_TEMPS; t++) {
    snprintf(names[t], sizeof(names[t]), SAFETY_TEMP_PREFIX "%s%u", tags[t],
             id);
    temps[t] = ir_operand_temp(names[t]);
  }
  IROperand *top = &temps[0];
  IROperand *rounds = &temps[1];
  IROperand *highest = &temps[2];
  IROperand *index = &temps[3];
  IROperand *scaled = &temps[4];
  IROperand *length = &temps[5];
  IROperand *runs = &temps[6];

  IROperand adjust = ir_operand_int(hoist->adjust);
  IROperand primary_step = ir_operand_int(hoist->primary_step);
  IROperand index_step = ir_operand_int(hoist->index_step);
  IROperand addend = ir_operand_int(hoist->addend);
  IROperand stride = ir_operand_int(hoist->stride);
  IROperand size = ir_operand_int(hoist->size);
  IROperand zero = ir_operand_int(0);
  int ok = 0;

  for (int t = 0; t < SAFETY_HOIST_TEMPS; t++) {
    if (!temps[t].name) {
      goto done;
    }
  }

  /* How far the tested variable gets. */
  if (!safety_emit_binary(out, hoist->location, "+", names[0], &hoist->bound,
                          &adjust, 0)) {
    goto done;
  }

  /* How many times the body runs, less one, and from that the last value the
   * indexing variable takes. Dividing is what pins the two counters together;
   * where the tested variable steps by one it is already the count. */
  const IROperand *last_index = top;
  if (hoist->primary_step > 1) {
    if (!safety_emit_binary(out, hoist->location, "/", names[1], top,
                            &primary_step, 0)) {
      goto done;
    }
    last_index = rounds;
  }
  if (hoist->index_step != 1 || last_index != top) {
    if (!safety_emit_binary(out, hoist->location, "*", names[2], last_index,
                            &index_step, 0)) {
      goto done;
    }
    last_index = highest;
  }

  if (hoist->addend != 0) {
    if (!safety_emit_binary(out, hoist->location, "+", names[3], last_index,
                            &addend, 0)) {
      goto done;
    }
    last_index = index;
  }

  if (!safety_emit_binary(out, hoist->location, "*", names[4], last_index,
                          &stride, 0) ||
      !safety_emit_binary(out, hoist->location, "+", names[5], scaled, &size,
                          0) ||
      !safety_emit_binary(out, hoist->location, ">=", names[6], top, &zero,
                          0)) {
    goto done;
  }

  char guarded[64];
  snprintf(guarded, sizeof(guarded), SAFETY_TEMP_PREFIX "hg%u", id);
  IROperand guarded_operand = ir_operand_temp(guarded);
  if (!guarded_operand.name ||
      !safety_emit_binary(out, hoist->location, "*", guarded, length, runs,
                          0)) {
    ir_operand_destroy(&guarded_operand);
    goto done;
  }

  IROperand arguments[5];
  if (!ir_operand_clone(&hoist->base, &arguments[0])) {
    ir_operand_destroy(&guarded_operand);
    goto done;
  }
  arguments[1] = ir_operand_int(0);
  arguments[2] = guarded_operand;
  arguments[3] = ir_operand_int(hoist->access_kind);
  arguments[4] = ir_operand_int((long long)hoist->location.line);
  ok = safety_emit_call(out, hoist->location, "mettle_safety_check", arguments,
                        5);
  ir_operand_destroy(&arguments[0]);
  ir_operand_destroy(&guarded_operand);

done:
  for (int t = 0; t < SAFETY_HOIST_TEMPS; t++) {
    ir_operand_destroy(&temps[t]);
  }
  return ok;
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

/* ---- resolving a pointer once, comparing per access ------------------------- */
/*
 * Where nothing about an index can be settled, the access still has to be
 * checked, and a check that walks the shadow map is a call and four dependent
 * loads. But a loop that indexes one pointer asks about the same allocation
 * every time, and how far that allocation runs is loop-invariant even when the
 * indices are not.
 *
 * So the allocation is resolved once in front of the loop, and each access
 * becomes `(unsigned)offset > span - size`, which is a subtract, a compare and
 * a branch that is never taken. Failing it is not a verdict: it calls the full
 * check, which is what keeps this exact for interior pointers reading
 * backwards, for dead allocations, and for anything else the comparison alone
 * cannot judge.
 *
 * This is what makes a checked heapsort possible. Its indices come out of
 * comparisons so nothing bounds them, and every access was paying for a walk
 * to be told the same thing about the same array.
 */

static int safety_expand_region(IRInstructionVector *out,
                                const SafetyAccess *access);

typedef struct {
  size_t header_index;
  IROperand base; /* owned */
  char temp[64];  /* the span this loop resolves once */
  SourceLocation location;
} SafetySpan;

/* Follow a base pointer back to the name it was copied from.
 *
 * Lowering reads a pointer into a fresh temporary at each use, so the operand
 * a check carries is defined inside the loop even when the pointer itself
 * never moves. Taken at face value that says the pointer changes every
 * iteration, and it was enough to decline the cheap form for every access in
 * base64_encode. Copies and pointer casts pass the same address along, so the
 * name behind them is what the span should be keyed on. */
static const IROperand *safety_base_root(const IRFunction *function,
                                         size_t before, const IROperand *base,
                                         const IROperand **delta_out,
                                         size_t *delta_from, int depth) {
  if (base->kind != IR_OPERAND_TEMP || !base->name || depth > 4) {
    return base;
  }
  const IRInstruction *producer =
      ir_find_temp_producer_before(function, before, base->name);
  if (!producer || producer->is_float) {
    return base;
  }
  size_t producer_index = (size_t)(producer - function->instructions);

  if (producer->op == IR_OP_ASSIGN || producer->op == IR_OP_CAST) {
    if (producer->lhs.kind != IR_OPERAND_SYMBOL &&
        producer->lhs.kind != IR_OPERAND_TEMP) {
      return base;
    }
    return safety_base_root(function, producer_index, &producer->lhs,
                            delta_out, delta_from, depth + 1);
  }

  /* `root + something`, where the something moves each iteration. The pointer
   * really does move, so it cannot be resolved once; but what it moves within
   * does not, so the comparison is made against the root's span with the
   * displacement folded into the offset. Only the fast comparison is rebased.
   * A failure still calls the check with the pointer the program actually
   * used, so nothing about what counts as a violation changes. */
  if (producer->op == IR_OP_BINARY && producer->text &&
      strcmp(producer->text, "+") == 0 && !*delta_out &&
      (producer->lhs.kind == IR_OPERAND_SYMBOL ||
       producer->lhs.kind == IR_OPERAND_TEMP)) {
    *delta_out = &producer->rhs;
    *delta_from = producer_index;
    return safety_base_root(function, producer_index, &producer->lhs,
                            delta_out, delta_from, depth + 1);
  }
  return base;
}

static int safety_operand_same(const IROperand *a, const IROperand *b) {
  if (a->kind != b->kind) {
    return 0;
  }
  if (a->kind == IR_OPERAND_INT) {
    return a->int_value == b->int_value;
  }
  return a->name && b->name && strcmp(a->name, b->name) == 0;
}

/* `span = mettle_safety_span(base)`, emitted in front of the loop. */
static int safety_emit_span_resolve(IRInstructionVector *out,
                                    const SafetySpan *span) {
  IRInstruction call = {0};
  call.op = IR_OP_CALL;
  call.location = span->location;
  call.text = mettle_strdup("mettle_safety_span");
  call.dest = ir_operand_temp(span->temp);
  call.arguments = calloc(1, sizeof(IROperand));
  if (!call.text || !call.dest.name || !call.arguments) {
    ir_instruction_destroy_storage(&call);
    return 0;
  }
  call.argument_count = 1;
  if (!ir_operand_clone(&span->base, &call.arguments[0]) ||
      !ir_instruction_vector_append_move(out, &call)) {
    ir_instruction_destroy_storage(&call);
    return 0;
  }
  return 1;
}

/* The access itself: compare against the resolved span, and only ask properly
 * when that comparison says something might be wrong. */
static int safety_emit_span_check(IRInstructionVector *out,
                                  const SafetyAccess *access,
                                  const char *span_temp,
                                  const IROperand *delta) {
  unsigned id = g_safety_next_id++;
  char limit[64];
  char total[64];
  char bad[64];
  char ok_label[64];
  snprintf(limit, sizeof(limit), SAFETY_TEMP_PREFIX "sl%u", id);
  snprintf(total, sizeof(total), SAFETY_TEMP_PREFIX "st%u", id);
  snprintf(bad, sizeof(bad), SAFETY_TEMP_PREFIX "sb%u", id);
  snprintf(ok_label, sizeof(ok_label), "ir_safe_in_%u", id);

  IROperand span_operand = ir_operand_temp(span_temp);
  IROperand limit_operand = ir_operand_temp(limit);
  IROperand total_operand = ir_operand_temp(total);
  IROperand size_operand = ir_operand_int(access->size);
  int ok = 0;

  if (!span_operand.name || !limit_operand.name || !total_operand.name) {
    goto done;
  }
  if (!safety_emit_binary(out, access->location, "-", limit, &span_operand,
                          &size_operand, 0)) {
    goto done;
  }

  /* Where the pointer was reached through arithmetic, the displacement joins
   * the offset so both are measured from the same root. */
  const IROperand *measured = access->offset;
  if (delta) {
    if (!safety_emit_binary(out, access->location, "+", total, access->offset,
                            delta, 0)) {
      goto done;
    }
    measured = &total_operand;
  }

  /* Comparing without sign is what covers both ends at once: a negative offset
   * reads as an enormous unsigned value and fails, which sends it to the full
   * check rather than rejecting it. */
  if (!safety_emit_binary(out, access->location, ">", bad, measured,
                          &limit_operand, 1) ||
      !safety_emit_branch_zero(out, access->location, bad, ok_label)) {
    goto done;
  }
  ok = safety_expand_region(out, access) &&
       safety_emit_label(out, access->location, ok_label);

done:
  ir_operand_destroy(&span_operand);
  ir_operand_destroy(&limit_operand);
  ir_operand_destroy(&total_operand);
  return ok;
}

/* ---- the loops, gathered once ----------------------------------------------- */

static const SafetyLoopForm *safety_enclosing_loop(const SafetyLoopList *loops,
                                                   size_t index) {
  for (size_t i = loops->count; i-- > 0;) {
    const SafetyLoopForm *loop = &loops->items[i];
    if (index > loop->bounds.branch_index && index < loop->bounds.jump_index) {
      return loop;
    }
  }
  return NULL;
}

static void safety_loop_list_destroy(SafetyLoopList *loops) {
  free(loops->items);
  loops->items = NULL;
  loops->count = 0;
  loops->capacity = 0;
}

/* Source order, so a backward scan meets the innermost enclosing loop first:
 * an inner loop's header comes after its outer loop's. */
static int safety_loop_list_build(IRFunction *function, SafetyLoopList *loops) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *instruction = &function->instructions[i];
    if (instruction->op != IR_OP_LABEL ||
        !ir_label_is_while_header(instruction->text)) {
      continue;
    }
    SafetyLoopForm form;
    if (!safety_parse_loop_form(function, i, &form)) {
      continue;
    }
    if (loops->count == loops->capacity) {
      size_t capacity = loops->capacity ? loops->capacity * 2 : 8;
      SafetyLoopForm *grown =
          realloc(loops->items, capacity * sizeof(SafetyLoopForm));
      if (!grown) {
        safety_loop_list_destroy(loops);
        return 0;
      }
      loops->items = grown;
      loops->capacity = capacity;
    }
    loops->items[loops->count++] = form;
  }
  return 1;
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

  /* Decide first, rewrite second. The proofs read the instructions that
   * produced a check's operands, and rewriting moves instructions out of the
   * array as it goes, so a proof running mid-rewrite would look back at
   * emptied slots and conclude it knows nothing. */
  enum {
    SAFETY_KEEP = 0,
    SAFETY_PROVED = 1,
    SAFETY_HOISTED = 2,
    SAFETY_SPANNED = 3
  };
  unsigned char *outcome = calloc(function->instruction_count, 1);
  SafetyHoist *hoists = calloc(check_count, sizeof(SafetyHoist));
  SafetySpan *spans = calloc(check_count, sizeof(SafetySpan));
  size_t *span_of = calloc(function->instruction_count, sizeof(size_t));
  const IROperand **span_delta =
      calloc(function->instruction_count, sizeof(const IROperand *));
  size_t hoist_count = 0;
  size_t span_count = 0;
  SafetyLoopList loops = {0};
  if (!outcome || !hoists || !spans || !span_of || !span_delta) {
    free(outcome);
    free(hoists);
    free(spans);
    free(span_of);
    free(span_delta);
    return 0;
  }
  if (!safety_loop_list_build(function, &loops)) {
    free(outcome);
    free(hoists);
    free(spans);
    free(span_of);
    free(span_delta);
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];
    if (instruction->op != IR_OP_SAFETY_CHECK) {
      continue;
    }
    SafetyAccess access;
    if (!safety_read(instruction, &access)) {
      goto fail;
    }
    if (safety_prove(function, &loops, i, &access)) {
      outcome[i] = SAFETY_PROVED;
      continue;
    }
    if (safety_try_hoist(function, &loops, i, &access, &hoists[hoist_count])) {
      hoist_count++;
      outcome[i] = SAFETY_HOISTED;
      continue;
    }

    /* Nothing settles the index, so the access keeps a check. But if it is in
     * a loop that cannot release what it is walking, and the pointer holds
     * still, resolving the allocation once turns the check from a call into a
     * comparison. */
    const SafetyLoopForm *loop = safety_enclosing_loop(&loops, i);
    if (!loop) {
      safety_trace("not in any loop, so there is nothing to resolve against",
                   access.location.line);
      continue;
    }
    if (!safety_body_has_no_calls(function, &loop->bounds)) {
      safety_trace("the loop calls out, so what it walks could be freed "
                   "under it",
                   access.location.line);
      continue;
    }
    const IROperand *delta = NULL;
    size_t delta_from = 0;
    const IROperand *root =
        safety_base_root(function, i, access.base, &delta, &delta_from, 0);
    if (!safety_operand_invariant_in(function, loop->header_index,
                                     loop->bounds.jump_index, root)) {
      safety_trace("the pointer moves inside the loop and is not a fixed one "
                   "displaced, so one resolution would not describe it",
                   access.location.line);
      continue;
    }
    /* The displacement is read again where the check sits, so it has to still
     * hold what it held where the pointer was formed. */
    if (delta && !safety_operand_invariant_in(function, delta_from + 1, i,
                                              delta)) {
      safety_trace("the displacement changes between forming the pointer and "
                   "using it",
                   access.location.line);
      continue;
    }

    size_t found = span_count;
    for (size_t s = 0; s < span_count; s++) {
      if (spans[s].header_index == loop->header_index &&
          safety_operand_same(&spans[s].base, root)) {
        found = s;
        break;
      }
    }
    if (found == span_count) {
      SafetySpan *fresh = &spans[span_count];
      fresh->header_index = loop->header_index;
      fresh->location = access.location;
      snprintf(fresh->temp, sizeof(fresh->temp), SAFETY_TEMP_PREFIX "sp%u",
               g_safety_next_id++);
      if (!ir_operand_clone(root, &fresh->base)) {
        goto fail;
      }
      span_count++;
    }
    span_of[i] = found;
    span_delta[i] = delta;
    outcome[i] = SAFETY_SPANNED;
  }
  safety_loop_list_destroy(&loops);

  IRInstructionVector out = {0};
  if (!ir_instruction_vector_reserve(&out, function->instruction_count + 16)) {
    goto fail;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];

    /* A loop's hoisted checks and resolved pointers go in front of its
     * header. */
    for (size_t h = 0; h < hoist_count; h++) {
      if (hoists[h].header_index == i &&
          !safety_emit_hoisted(&out, &hoists[h])) {
        ir_instruction_vector_destroy(&out);
        goto fail;
      }
    }
    for (size_t s = 0; s < span_count; s++) {
      if (spans[s].header_index == i &&
          !safety_emit_span_resolve(&out, &spans[s])) {
        ir_instruction_vector_destroy(&out);
        goto fail;
      }
    }

    if (instruction->op != IR_OP_SAFETY_CHECK) {
      if (!ir_instruction_vector_append_move(&out, instruction)) {
        ir_instruction_vector_destroy(&out);
        goto fail;
      }
      continue;
    }

    SafetyAccess access;
    if (!safety_read(instruction, &access)) {
      ir_instruction_vector_destroy(&out);
      goto fail;
    }
    if (stats) {
      stats->emitted++;
    }

    if (outcome[i] == SAFETY_PROVED) {
      if (stats) {
        stats->proved++;
      }
      continue;
    }
    if (outcome[i] == SAFETY_HOISTED) {
      if (stats) {
        stats->hoisted++;
      }
      continue;
    }
    if (outcome[i] == SAFETY_SPANNED) {
      if (!safety_emit_span_check(&out, &access, spans[span_of[i]].temp,
                                  span_delta[i])) {
        ir_instruction_vector_destroy(&out);
        goto fail;
      }
      if (stats) {
        stats->spanned++;
      }
      ir_explain_safety_note(access.location.filename, access.location.line,
                             function->name, IR_SAFETY_SURVIVOR_SPAN);
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
      goto fail;
    }
    /* The check is not moved into `out`: its operands were cloned into the
     * replacement, and ir_function_replace_instructions frees what is left of
     * the old array below. */
  }

  for (size_t h = 0; h < hoist_count; h++) {
    ir_operand_destroy(&hoists[h].base);
    ir_operand_destroy(&hoists[h].bound);
  }
  for (size_t s = 0; s < span_count; s++) {
    ir_operand_destroy(&spans[s].base);
  }
  free(spans);
  free(span_of);
  free(span_delta);
  free(hoists);
  free(outcome);
  if (!ir_function_replace_instructions(function, &out)) {
    ir_instruction_vector_destroy(&out);
    return 0;
  }
  return 1;

fail:
  safety_loop_list_destroy(&loops);
  for (size_t h = 0; h < hoist_count; h++) {
    ir_operand_destroy(&hoists[h].base);
    ir_operand_destroy(&hoists[h].bound);
  }
  for (size_t s = 0; s < span_count; s++) {
    ir_operand_destroy(&spans[s].base);
  }
  free(spans);
  free(span_of);
  free(span_delta);
  free(hoists);
  free(outcome);
  return 0;
}

int ir_safety_resolve_program(IRProgram *program, IRSafetyStats *stats) {
  if (!program) {
    return 1;
  }
  clock_t started = safety_time_enabled() ? clock() : 0;
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
  if (safety_time_enabled()) {
    /* Ticks rather than a converted figure: clock()'s units do not reliably
     * match CLOCKS_PER_SEC across the toolchains this builds with, and a
     * number in the wrong units is worse than none. Runs are comparable, which
     * is what this is for. */
    fprintf(stderr, "safety: resolving took %lld ticks\n",
            (long long)(clock() - started));
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

/* ---- describing the globals ------------------------------------------------ */

/* Module variables sit at fixed addresses for the whole run, so one sweep at
 * the top of `main` describes them all and nothing ever retires them.
 *
 * This only matters for a pointer taken into a global and carried somewhere
 * else. Indexing one directly never reaches the map at all: the size is right
 * there in the program, so the check is a comparison against a constant, or is
 * proved away outright. */
static int safety_describe_globals(IRProgram *program, IRFunction *entry) {
  size_t described = 0;
  for (size_t i = 0; i < program->module_symbol_count; i++) {
    const IRModuleSymbol *symbol = &program->module_symbols[i];
    if (symbol->kind == IR_MODSYM_VARIABLE && !symbol->is_extern &&
        symbol->name && symbol->type && symbol->type->size > 0) {
      described++;
    }
  }
  if (described == 0) {
    return 1;
  }

  IRInstructionVector out = {0};
  if (!ir_instruction_vector_reserve(&out,
                                     entry->instruction_count + described * 2)) {
    return 0;
  }

  for (size_t i = 0; i < program->module_symbol_count; i++) {
    const IRModuleSymbol *symbol = &program->module_symbols[i];
    if (symbol->kind != IR_MODSYM_VARIABLE || symbol->is_extern ||
        !symbol->name || !symbol->type || symbol->type->size == 0) {
      continue;
    }

    char address[96];
    snprintf(address, sizeof(address), SAFETY_TEMP_PREFIX "g%u",
             g_safety_next_id++);
    IROperand size_operand = ir_operand_int((long long)symbol->type->size);

    /* The instruction gets its own copies: appending moves it into the vector,
     * which then owns whatever names it holds. */
    IRInstruction take = {0};
    take.op = IR_OP_ADDRESS_OF;
    take.location = entry->location;
    take.dest = ir_operand_temp(address);
    take.lhs = ir_operand_symbol(symbol->name);
    if (!take.dest.name || !take.lhs.name) {
      ir_instruction_destroy_storage(&take);
      ir_instruction_vector_destroy(&out);
      return 0;
    }
    if (!ir_instruction_vector_append_move(&out, &take)) {
      ir_instruction_destroy_storage(&take);
      ir_instruction_vector_destroy(&out);
      return 0;
    }

    IROperand address_operand = ir_operand_temp(address);
    int ok = address_operand.name &&
             safety_emit_register(&out, entry->location, &address_operand,
                                  &size_operand);
    ir_operand_destroy(&address_operand);
    if (!ok) {
      ir_instruction_vector_destroy(&out);
      return 0;
    }
  }

  for (size_t i = 0; i < entry->instruction_count; i++) {
    if (!ir_instruction_vector_append_move(&out, &entry->instructions[i])) {
      ir_instruction_vector_destroy(&out);
      return 0;
    }
  }
  if (!ir_function_replace_instructions(entry, &out)) {
    ir_instruction_vector_destroy(&out);
    return 0;
  }
  ir_function_clear_cfg(entry);
  return 1;
}

int ir_safety_register_allocations(IRProgram *program) {
  if (!program) {
    return 1;
  }
  const char *allocator_source = safety_allocator_source(program);
  IRFunction *entry = NULL;
  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *function = program->functions[i];
    if (!function) {
      continue;
    }
    if (function->name && strcmp(function->name, "main") == 0) {
      entry = function;
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
  if (entry && !safety_describe_globals(program, entry)) {
    return 0;
  }
  return 1;
}
