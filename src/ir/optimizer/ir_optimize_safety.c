/* `--safe`: resolving the access marks lowering left behind.
 *
 * Every IR_OP_SAFETY_CHECK is either deleted, because the access provably
 * cannot leave its object, or rewritten into ordinary IR. Nothing downstream
 * sees the opcode. See ir_safety.h for why this runs before the optimizer. */

#include "ir_optimize_internal.h"
#include "../ir_safety.h"

/* Distinct from lowering's ".t%d" temps and from every label prefix the
 * recognizers match on, so a resolved check can never be mistaken for one. */
#define SAFETY_TEMP_PREFIX ".safe"
#define SAFETY_LABEL_PREFIX "ir_safe_ok_"

static unsigned g_safety_next_id;

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
  insn.arguments = calloc(count, sizeof(IROperand));
  if (!insn.text || !insn.arguments) {
    ir_instruction_destroy_storage(&insn);
    return 0;
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
  IROperand arguments[6];
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
  arguments[4] = ir_operand_string(access->what);
  arguments[5] = ir_operand_int((long long)access->location.line);
  built = 6;
  if (arguments[4].kind != IR_OPERAND_STRING) {
    goto done;
  }

  ok = safety_emit_call(out, access->location, "mettle_safety_check", arguments,
                        6);

done:
  for (size_t i = 0; i < built; i++) {
    ir_operand_destroy(&arguments[i]);
  }
  return ok;
}

/* ---- driver ---------------------------------------------------------------- */

static int safety_resolve_function(IRFunction *function, IRSafetyStats *stats) {
  int found = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_SAFETY_CHECK) {
      found = 1;
      break;
    }
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
    if (instruction->op != IR_OP_SAFETY_CHECK) {
      if (!ir_instruction_vector_append_move(&out, instruction)) {
        ir_instruction_vector_destroy(&out);
        return 0;
      }
      continue;
    }

    SafetyAccess access;
    if (!safety_read(instruction, &access)) {
      ir_instruction_vector_destroy(&out);
      return 0;
    }
    if (stats) {
      stats->emitted++;
    }

    int expanded;
    if (access.extent == IR_SAFETY_EXTENT_UNKNOWN) {
      expanded = safety_expand_region(&out, &access);
      if (expanded && stats) {
        stats->region_calls++;
      }
    } else {
      expanded = safety_expand_extent(&out, &access);
      if (expanded && stats) {
        stats->extent_tests++;
      }
    }
    if (!expanded) {
      ir_instruction_vector_destroy(&out);
      return 0;
    }
    /* The check is not moved into `out`: its operands were cloned into the
     * replacement, and ir_function_replace_instructions frees what is left of
     * the old array below. */
  }

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
  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *function = program->functions[i];
    if (function && !safety_resolve_function(function, stats)) {
      return 0;
    }
  }
  return 1;
}
