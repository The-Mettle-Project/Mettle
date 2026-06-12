#include "ir_debug_hooks.h"
#include "../common.h"
#include "ir_profile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --debug-hooks: source-level debugger instrumentation.
 *
 * Modeled on ir_profile.c, but for the interactive debugger runtime
 * (src/runtime/debug.c) instead of the profiler:
 *
 *   mettle_dbg_enter(fn_id)            function entry (shadow call stack push)
 *   mettle_dbg_exit()                  before every return (pop)
 *   mettle_dbg_line(line)              first instruction of each source line
 *                                      (breakpoint / step check)
 *   mettle_dbg_local(name, type, &sym, is_param)
 *                                      registers a LIVE POINTER to a local or
 *                                      parameter, so the paused runtime can
 *                                      read -- and write -- its current value
 *
 * The &sym argument is the whole trick: a real IR_OP_ADDRESS_OF forces the
 * backend to give the variable a memory home (MIR force-spills
 * address-taken locals), so values read through the pointer are always
 * current. No stack-layout plumbing, backend-agnostic by construction.
 *
 * Function identity reuses the profile registry (ir_profile_registry_add),
 * so the embedded mettle_profile_names/files/lines tables describe debug
 * functions too -- the runtime sends those tables to the adapter on connect.
 *
 * Designed for -O0 builds (the debugger never passes --release): no inlining,
 * source instruction order preserved. */

static size_t g_dbg_temp_counter = 0;

static int ir_dbg_should_instrument(const IRFunction *function) {
  const char *name = NULL;
  if (!function || !function->name || function->instruction_count == 0) {
    return 0;
  }
  name = function->name;
  if (strncmp(name, "mettle_dbg_", 11) == 0 ||
      strncmp(name, "mettle_profile_", 15) == 0 ||
      strncmp(name, "__inl_", 6) == 0) {
    return 0;
  }
  return 1;
}

static int ir_dbg_build_call(IRInstruction *instruction, const char *callee,
                             SourceLocation location, size_t argument_count) {
  memset(instruction, 0, sizeof(*instruction));
  instruction->op = IR_OP_CALL;
  instruction->location = location;
  instruction->text = mettle_strdup(callee);
  if (!instruction->text) {
    return 0;
  }
  if (argument_count > 0) {
    instruction->arguments = calloc(argument_count, sizeof(IROperand));
    if (!instruction->arguments) {
      free(instruction->text);
      instruction->text = NULL;
      return 0;
    }
    instruction->argument_count = argument_count;
  }
  return 1;
}

static void ir_dbg_destroy_instruction(IRInstruction *instruction) {
  if (!instruction) {
    return;
  }
  for (size_t i = 0; i < instruction->argument_count; i++) {
    ir_operand_destroy(&instruction->arguments[i]);
  }
  free(instruction->arguments);
  instruction->arguments = NULL;
  instruction->argument_count = 0;
  ir_operand_destroy(&instruction->dest);
  ir_operand_destroy(&instruction->lhs);
  free(instruction->text);
  instruction->text = NULL;
}

/* Register a variable's name/type in the program-level table the backend
 * embeds as mettle_dbg_local_names/types. The hook passes the returned
 * index -- string-literal call arguments are avoided entirely because their
 * ABI differs between the MIR and fallback backends. Returns the index, or
 * (size_t)-1 on failure. */
static size_t ir_dbg_local_registry_add(IRProgram *program, const char *name,
                                        const char *type_name) {
  if (program->debug_local_entry_count >= program->debug_local_entry_capacity) {
    size_t new_capacity = program->debug_local_entry_capacity == 0
                              ? 64u
                              : program->debug_local_entry_capacity * 2u;
    IRDebugLocalEntry *grown =
        realloc(program->debug_local_entries,
                new_capacity * sizeof(IRDebugLocalEntry));
    if (!grown) {
      return (size_t)-1;
    }
    program->debug_local_entries = grown;
    program->debug_local_entry_capacity = new_capacity;
  }
  IRDebugLocalEntry *entry =
      &program->debug_local_entries[program->debug_local_entry_count];
  entry->name = mettle_strdup(name);
  entry->type_name = mettle_strdup(type_name ? type_name : "?");
  if (!entry->name || !entry->type_name) {
    free(entry->name);
    free(entry->type_name);
    return (size_t)-1;
  }
  return program->debug_local_entry_count++;
}

/* Insert, at `index`:
 *   .dbgN <- &symbol
 *   call mettle_dbg_local(local_id, .dbgN, is_param)
 * Returns the number of instructions inserted (2), or 0 on failure. */
static size_t ir_dbg_insert_local_registration(IRProgram *program,
                                               IRFunction *function,
                                               size_t index, const char *name,
                                               const char *type_name,
                                               int is_param,
                                               SourceLocation location) {
  char temp_name[32];
  IRInstruction addr = {0};
  IRInstruction call = {0};

  if (!name) {
    return 0;
  }
  size_t local_id = ir_dbg_local_registry_add(program, name, type_name);
  if (local_id == (size_t)-1) {
    return 0;
  }
  snprintf(temp_name, sizeof(temp_name), ".dbg%zu", g_dbg_temp_counter++);

  memset(&addr, 0, sizeof(addr));
  addr.op = IR_OP_ADDRESS_OF;
  addr.location = location;
  addr.dest = ir_operand_temp(temp_name);
  addr.lhs = ir_operand_symbol(name);
  if (addr.dest.kind != IR_OPERAND_TEMP || addr.lhs.kind != IR_OPERAND_SYMBOL) {
    ir_dbg_destroy_instruction(&addr);
    return 0;
  }

  if (!ir_dbg_build_call(&call, "mettle_dbg_local", location, 3)) {
    ir_dbg_destroy_instruction(&addr);
    return 0;
  }
  call.arguments[0] = ir_operand_int((long long)local_id);
  call.arguments[1] = ir_operand_temp(temp_name);
  call.arguments[2] = ir_operand_int(is_param ? 1 : 0);
  if (call.arguments[1].kind != IR_OPERAND_TEMP) {
    ir_dbg_destroy_instruction(&addr);
    ir_dbg_destroy_instruction(&call);
    return 0;
  }

  if (!ir_function_insert_instruction(function, index, &addr)) {
    ir_dbg_destroy_instruction(&addr);
    ir_dbg_destroy_instruction(&call);
    return 0;
  }
  ir_dbg_destroy_instruction(&addr);
  if (!ir_function_insert_instruction(function, index + 1, &call)) {
    ir_dbg_destroy_instruction(&call);
    return 0;
  }
  ir_dbg_destroy_instruction(&call);
  return 2;
}

static int ir_dbg_insert_simple_call(IRFunction *function, size_t index,
                                     const char *callee, long long arg0,
                                     int has_arg, SourceLocation location) {
  IRInstruction call = {0};
  if (!ir_dbg_build_call(&call, callee, location, has_arg ? 1u : 0u)) {
    return 0;
  }
  if (has_arg) {
    call.arguments[0] = ir_operand_int(arg0);
  }
  if (!ir_function_insert_instruction(function, index, &call)) {
    ir_dbg_destroy_instruction(&call);
    return 0;
  }
  ir_dbg_destroy_instruction(&call);
  return 1;
}

static SourceLocation ir_dbg_function_location(const IRFunction *function) {
  SourceLocation location = {0};
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].location.line > 0) {
      return function->instructions[i].location;
    }
  }
  return location;
}

static int ir_dbg_instrument_function(IRProgram *program,
                                      IRFunction *function) {
  SourceLocation entry_location = ir_dbg_function_location(function);
  uint32_t fn_id = ir_profile_registry_add(
      program, function->name, entry_location.filename, entry_location.line);
  if (fn_id == IR_PROFILE_ID_NONE) {
    return 0;
  }
  function->profile_id = fn_id;

  /* Entry hook + parameter registrations, in order, at the top. */
  size_t insert_at = 0;
  if (!ir_dbg_insert_simple_call(function, insert_at++, "mettle_dbg_enter",
                                 (long long)fn_id, 1, entry_location)) {
    return 0;
  }
  for (size_t p = 0; p < function->parameter_count; p++) {
    const char *pname =
        function->parameter_names ? function->parameter_names[p] : NULL;
    const char *ptype =
        function->parameter_types ? function->parameter_types[p] : NULL;
    if (!pname) {
      continue;
    }
    size_t inserted = ir_dbg_insert_local_registration(
        program, function, insert_at, pname, ptype, 1, entry_location);
    if (inserted == 0) {
      return 0;
    }
    insert_at += inserted;
  }

  /* Walk the body: exit hooks before returns, registrations after local
   * declarations, line hooks at each source-line change. The walk maintains
   * its own index because every insertion shifts the remainder. */
  size_t last_hooked_line = 0;
  for (size_t i = insert_at; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];

    if (instruction->op == IR_OP_LABEL || instruction->op == IR_OP_NOP) {
      continue;
    }

    /* Line hook first -- before the first non-label instruction of each new
     * source line, INCLUDING returns and declarations, so `return x;` lines
     * are breakable. Labels are skipped above so a hook never lands on the
     * fall-through side of a jump target. */
    if (instruction->location.line > 0 &&
        instruction->location.line != last_hooked_line) {
      last_hooked_line = instruction->location.line;
      if (!ir_dbg_insert_simple_call(function, i, "mettle_dbg_line",
                                     (long long)instruction->location.line, 1,
                                     instruction->location)) {
        return 0;
      }
      i++; /* past the inserted hook, back onto the original instruction */
      instruction = &function->instructions[i]; /* array may have realloc'd */
    }

    if (instruction->op == IR_OP_RETURN) {
      if (!ir_dbg_insert_simple_call(function, i, "mettle_dbg_exit", 0, 0,
                                     instruction->location)) {
        return 0;
      }
      i++; /* skip over the inserted call */
      continue;
    }

    if (instruction->op == IR_OP_DECLARE_LOCAL && instruction->dest.name) {
      char *local_name = instruction->dest.name;
      char *local_type = instruction->text;
      SourceLocation loc = instruction->location;
      size_t inserted = ir_dbg_insert_local_registration(
          program, function, i + 1, local_name, local_type, 0, loc);
      if (inserted == 0) {
        return 0;
      }
      i += inserted;
      continue;
    }
  }

  return 1;
}

int ir_debug_hooks_instrument_program(IRProgram *program) {
  if (!program) {
    return 0;
  }

  program->profile_entry_count = 0;
  program->debug_local_entry_count = 0;
  g_dbg_temp_counter = 0;

  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *function = program->functions[i];
    if (!function || !ir_dbg_should_instrument(function)) {
      if (function) {
        function->profile_id = IR_PROFILE_ID_NONE;
      }
      continue;
    }
    if (!ir_dbg_instrument_function(program, function)) {
      return 0;
    }
  }

  return 1;
}
