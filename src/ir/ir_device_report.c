#include "ir_device_report.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEVICE_INDEX_LINES 3
#define DEVICE_ROOT_STEPS 24

typedef struct {
  const char **names;
  size_t count;
  size_t capacity;
} DeviceNames;

typedef struct {
  size_t index_lines[DEVICE_INDEX_LINES];
  size_t index_listed;
  size_t index_sites;
  int called;
  size_t call_line;
  const char *callee;
  int stored;
  size_t stored_line;
  int other;
  size_t other_line;
  int taken;
  size_t taken_line;
} DeviceUses;

static int device_names_hold(const DeviceNames *set, const IROperand *operand) {
  if (!operand || !operand->name ||
      (operand->kind != IR_OPERAND_TEMP &&
       operand->kind != IR_OPERAND_SYMBOL)) {
    return 0;
  }
  for (size_t i = 0; i < set->count; i++) {
    if (strcmp(set->names[i], operand->name) == 0) {
      return 1;
    }
  }
  return 0;
}

static void device_names_add(DeviceNames *set, const char *name) {
  if (!name) {
    return;
  }
  for (size_t i = 0; i < set->count; i++) {
    if (strcmp(set->names[i], name) == 0) {
      return;
    }
  }
  if (set->count == set->capacity) {
    size_t capacity = set->capacity ? set->capacity * 2 : 8;
    const char **grown = realloc(set->names, capacity * sizeof(*grown));
    if (!grown) {
      return;
    }
    set->names = grown;
    set->capacity = capacity;
  }
  set->names[set->count++] = name;
}

static void device_note_index(DeviceUses *uses, size_t line) {
  uses->index_sites++;
  for (size_t i = 0; i < uses->index_listed; i++) {
    if (uses->index_lines[i] == line) {
      return;
    }
  }
  if (uses->index_listed < DEVICE_INDEX_LINES) {
    uses->index_lines[uses->index_listed++] = line;
  }
}

static int device_is_offset(const IRInstruction *in) {
  return in->op == IR_OP_BINARY && in->text &&
         (strcmp(in->text, "+") == 0 || strcmp(in->text, "-") == 0);
}

static void device_scan_offset(const IRInstruction *in, DeviceNames *set,
                               DeviceUses *uses) {
  int through_lhs = device_names_hold(set, &in->lhs);
  const IROperand *other = through_lhs ? &in->rhs : &in->lhs;
  if (other->kind != IR_OPERAND_INT) {
    device_note_index(uses, in->location.line);
  }
  device_names_add(set, in->dest.name);
}

static void device_scan_call(const IRInstruction *in, const DeviceNames *set,
                             DeviceUses *uses) {
  for (size_t a = 0; a < in->argument_count; a++) {
    if (device_names_hold(set, &in->arguments[a]) && !uses->called) {
      uses->called = 1;
      uses->call_line = in->location.line;
      uses->callee = in->text;
    }
  }
}

static void device_scan_use(const IRInstruction *in, DeviceNames *set,
                            DeviceUses *uses) {
  int lhs = device_names_hold(set, &in->lhs);
  int rhs = device_names_hold(set, &in->rhs);
  switch (in->op) {
  case IR_OP_LOAD:
    return;
  case IR_OP_STORE:
    if (lhs && !uses->stored) {
      uses->stored = 1;
      uses->stored_line = in->location.line;
    }
    return;
  case IR_OP_ASSIGN:
  case IR_OP_CAST:
    if (lhs) {
      device_names_add(set, in->dest.name);
    }
    return;
  case IR_OP_CALL:
  case IR_OP_CALL_INDIRECT:
    device_scan_call(in, set, uses);
    return;
  default:
    break;
  }
  if (device_is_offset(in) && (lhs || rhs)) {
    device_scan_offset(in, set, uses);
    return;
  }
  if ((lhs || rhs) && !uses->other) {
    uses->other = 1;
    uses->other_line = in->location.line;
  }
}

static void device_collect_uses(const IRFunction *function, const char *local,
                                DeviceUses *uses) {
  DeviceNames set = {0};
  memset(uses, 0, sizeof(*uses));
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op == IR_OP_ADDRESS_SPACE_ALLOC && in->dest.name &&
        strcmp(in->dest.name, local) == 0) {
      device_names_add(&set, in->dest.name);
      continue;
    }
    if (in->op == IR_OP_ADDRESS_OF && in->lhs.kind == IR_OPERAND_SYMBOL &&
        in->lhs.name && strcmp(in->lhs.name, local) == 0) {
      if (!uses->taken) {
        uses->taken = 1;
        uses->taken_line = in->location.line;
      }
      device_names_add(&set, in->dest.name);
      continue;
    }
    if (set.count) {
      device_scan_use(in, &set, uses);
    }
  }
  free(set.names);
}

static void device_index_reason(const DeviceUses *uses, char *reason,
                                size_t size) {
  size_t more = 0;
  size_t distinct = uses->index_listed;
  if (uses->index_sites > distinct && distinct == DEVICE_INDEX_LINES) {
    more = 1;
  }
  if (distinct == 1) {
    snprintf(reason, size,
             "line %zu indexes it with a value that is not a constant",
             uses->index_lines[0]);
  } else if (distinct == 2) {
    snprintf(reason, size,
             "lines %zu and %zu index it with values that are not constants",
             uses->index_lines[0], uses->index_lines[1]);
  } else {
    snprintf(reason, size,
             "lines %zu, %zu and %zu%s index it with values that are not "
             "constants",
             uses->index_lines[0], uses->index_lines[1], uses->index_lines[2],
             more ? ", with others," : "");
  }
}

static void device_cause(char *cause, size_t size, size_t line,
                         const char *active, const char *passive,
                         const char *callee) {
  char target[160] = "";
  if (callee) {
    snprintf(target, sizeof(target), " `%s`", callee);
  }
  if (line) {
    snprintf(cause, size, "line %zu %s%s", line, active, target);
  } else {
    snprintf(cause, size, "%s%s", passive, target);
  }
}

int ir_device_report_storage_reason(const IRFunction *function,
                                    const char *local, int is_aggregate,
                                    char *reason, size_t size) {
  DeviceUses uses;
  char shown[128];
  char cause[256];
  if (!reason || !size) {
    return 0;
  }
  reason[0] = '\0';
  if (!function || !local) {
    return 0;
  }
  device_collect_uses(function, local, &uses);
  cause[0] = '\0';
  if (uses.called && uses.callee) {
    ir_display_name(uses.callee, shown, sizeof(shown));
    device_cause(cause, sizeof(cause), uses.call_line,
                 "passes its address to", "its address is passed to", shown);
  } else if (uses.stored) {
    device_cause(cause, sizeof(cause), uses.stored_line, "stores its address",
                 "its address is stored", NULL);
  } else if (uses.other) {
    device_cause(cause, sizeof(cause), uses.other_line,
                 "uses its address as a value",
                 "its address is used as a value", NULL);
  } else if (uses.index_listed) {
    device_index_reason(&uses, cause, sizeof(cause));
  } else if (!is_aggregate && uses.taken) {
    device_cause(cause, sizeof(cause), uses.taken_line, "takes its address",
                 "its address is taken", NULL);
  }
  if (!cause[0]) {
    return 0;
  }
  snprintf(reason, size, "%s", cause);
  return 1;
}

static const IRInstruction *device_producer(const IRFunction *function,
                                            size_t before, const char *name) {
  for (size_t i = before; i > 0; i--) {
    const IRInstruction *in = &function->instructions[i - 1];
    if (in->dest.name && strcmp(in->dest.name, name) == 0 &&
        ir_instruction_writes_destination(in)) {
      return in;
    }
  }
  return NULL;
}

static const IROperand *device_pointer_side(const IRInstruction *in) {
  if (in->lhs.kind == IR_OPERAND_SYMBOL) {
    return &in->lhs;
  }
  if (in->rhs.kind == IR_OPERAND_SYMBOL && in->lhs.kind != IR_OPERAND_TEMP) {
    return &in->rhs;
  }
  if (in->lhs.kind == IR_OPERAND_TEMP) {
    return &in->lhs;
  }
  return in->rhs.kind == IR_OPERAND_TEMP ? &in->rhs : NULL;
}

const char *ir_device_report_address_root(const IRFunction *function,
                                          size_t index,
                                          const IROperand *address) {
  const IROperand *current = address;
  size_t before = index;
  for (int step = 0; step < DEVICE_ROOT_STEPS && function && current; step++) {
    const IRInstruction *producer;
    if (current->kind == IR_OPERAND_SYMBOL) {
      return current->name;
    }
    if (current->kind != IR_OPERAND_TEMP || !current->name) {
      return NULL;
    }
    producer = device_producer(function, before, current->name);
    if (!producer) {
      return NULL;
    }
    before = (size_t)(producer - function->instructions);
    if (device_is_offset(producer)) {
      current = device_pointer_side(producer);
    } else if (producer->op == IR_OP_ASSIGN || producer->op == IR_OP_CAST ||
               producer->op == IR_OP_ADDRESS_OF) {
      current = &producer->lhs;
    } else {
      return NULL;
    }
  }
  return NULL;
}

size_t ir_device_report_code_line(const IRFunction *function,
                                  const IRInstruction *instruction) {
  const char *home = function ? function->location.filename : NULL;
  const char *file = instruction ? instruction->location.filename : NULL;
  if (!instruction) {
    return 0;
  }
  if (home && file && strcmp(home, file) != 0) {
    return 0;
  }
  return instruction->location.line;
}
