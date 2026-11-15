#include "ir_optimize_internal.h"

enum { IR_DOT_STEPS = 64, IR_DOT_NAMES = 96 };

typedef enum {
  IR_DOT_NONE = 0,
  IR_DOT_DATA,
  IR_DOT_PRODUCT,
  IR_DOT_ELEM,
  IR_DOT_INDEX,
  IR_DOT_ACC
} IRDotKind;

typedef enum {
  IR_DOT_OP_LOAD = 0,
  IR_DOT_OP_CAST,
  IR_DOT_OP_ADD,
  IR_DOT_OP_SUB,
  IR_DOT_OP_MUL,
  IR_DOT_OP_AND,
  IR_DOT_OP_OR,
  IR_DOT_OP_XOR,
  IR_DOT_OP_SHL,
  IR_DOT_OP_SHR,
  IR_DOT_OP_SAR,
  IR_DOT_OP_UNKNOWN
} IRDotOp;

typedef struct {
  IRDotKind kind;
  int side;
  int is_const;
  int slot;
  long long scale;
  long long value;
} IRDotValue;

typedef struct {
  IRDotOp op;
  int bits;
  int is_unsigned;
  int side;
  int a;
  int b;
  long long a_const;
  long long b_const;
} IRDotStep;

typedef struct {
  const IRFunction *function;
  const char *iv;
  const char *acc;
  const char *base[2];
  int elem_size;
  int acc_bits;
  const char *name[IR_DOT_NAMES];
  unsigned char name_is_symbol[IR_DOT_NAMES];
  IRDotValue bound[IR_DOT_NAMES];
  int name_count;
  IRDotValue iv_value;
  IRDotValue acc_value;
  IRDotStep step[IR_DOT_STEPS];
  int step_count;
} IRDotBody;

typedef struct {
  size_t header;
  size_t branch;
  size_t jump;
  const char *iv;
  const IROperand *bound;
} IRDotLoop;

static const struct {
  const char *name;
  int bits;
  int is_unsigned;
} IR_DOT_INT_TYPES[] = {{"int8", 8, 0},    {"int16", 16, 0},  {"int32", 32, 0},
                        {"int64", 64, 0},  {"uint8", 8, 1},   {"uint16", 16, 1},
                        {"uint32", 32, 1}, {"uint64", 64, 1}};

static const struct {
  const char *text;
  IRDotOp op;
} IR_DOT_BINARY_OPS[] = {{"+", IR_DOT_OP_ADD}, {"-", IR_DOT_OP_SUB},
                         {"*", IR_DOT_OP_MUL}, {"&", IR_DOT_OP_AND},
                         {"|", IR_DOT_OP_OR},  {"^", IR_DOT_OP_XOR},
                         {"<<", IR_DOT_OP_SHL}};

static int ir_dot_int_type(const char *text, int *bits, int *is_unsigned) {
  size_t count = sizeof(IR_DOT_INT_TYPES) / sizeof(IR_DOT_INT_TYPES[0]);
  for (size_t k = 0; text && k < count; k++) {
    if (strcmp(text, IR_DOT_INT_TYPES[k].name) == 0) {
      *bits = IR_DOT_INT_TYPES[k].bits;
      *is_unsigned = IR_DOT_INT_TYPES[k].is_unsigned;
      return 1;
    }
  }
  return 0;
}

static int ir_dot_type_is_pointer(const char *text) {
  size_t len = text ? strlen(text) : 0;
  return len > 0 && (text[len - 1] == '*' || strcmp(text, "cstring") == 0);
}

static const char *ir_dot_symbol_type(const IRFunction *function,
                                      const char *name) {
  const char *type = ir_function_local_declared_type(function, name);
  for (size_t p = 0; !type && p < function->parameter_count; p++) {
    if (function->parameter_names && function->parameter_names[p] &&
        function->parameter_types &&
        strcmp(function->parameter_names[p], name) == 0) {
      type = function->parameter_types[p];
    }
  }
  return type;
}

static long long ir_dot_narrow(long long value, int bits, int is_unsigned) {
  unsigned long long mask;
  unsigned long long low;
  if (bits >= 64) {
    return value;
  }
  mask = (1ULL << bits) - 1;
  low = (unsigned long long)value & mask;
  return !is_unsigned && (low >> (bits - 1)) ? (long long)(low | ~mask)
                                             : (long long)low;
}

static long long ir_dot_apply(IRDotOp op, long long a, long long b) {
  unsigned long long ua = (unsigned long long)a;
  unsigned long long ub = (unsigned long long)b;
  switch (op) {
  case IR_DOT_OP_ADD: return (long long)(ua + ub);
  case IR_DOT_OP_SUB: return (long long)(ua - ub);
  case IR_DOT_OP_MUL: return (long long)(ua * ub);
  case IR_DOT_OP_AND: return (long long)(ua & ub);
  case IR_DOT_OP_OR:  return (long long)(ua | ub);
  case IR_DOT_OP_XOR: return (long long)(ua ^ ub);
  case IR_DOT_OP_SHL: return (long long)(ua << (ub & 63));
  case IR_DOT_OP_SHR: return (long long)(ua >> (ub & 63));
  default:            return a >> (ub & 63);
  }
}

static IRDotOp ir_dot_binary_op(const IRInstruction *ins) {
  size_t count = sizeof(IR_DOT_BINARY_OPS) / sizeof(IR_DOT_BINARY_OPS[0]);
  if (ins->is_float || !ins->text) {
    return IR_DOT_OP_UNKNOWN;
  }
  for (size_t k = 0; k < count; k++) {
    if (strcmp(ins->text, IR_DOT_BINARY_OPS[k].text) == 0) {
      return IR_DOT_BINARY_OPS[k].op;
    }
  }
  if (strcmp(ins->text, ">>") == 0) {
    return ins->is_unsigned ? IR_DOT_OP_SHR : IR_DOT_OP_SAR;
  }
  return IR_DOT_OP_UNKNOWN;
}

static IRDotValue ir_dot_const(long long value) {
  IRDotValue v = {IR_DOT_DATA, 0, 1, -1, 0, value};
  return v;
}

static IRDotValue ir_dot_index(int side, long long scale, long long value) {
  IRDotValue v = {IR_DOT_INDEX, side, 1, -1, scale, value};
  return v;
}

static int ir_dot_emit(IRDotBody *body, IRDotStep step, IRDotValue *out) {
  IRDotValue value = {IR_DOT_DATA, 0, 0, body->step_count, 0, 0};
  if (body->step_count >= IR_DOT_STEPS) {
    return 0;
  }
  body->step[body->step_count++] = step;
  *out = value;
  return 1;
}

static int ir_dot_data_op(IRDotBody *body, IRDotOp op, const IRDotValue *a,
                          const IRDotValue *b, IRDotValue *out) {
  IRDotStep step = {op, 64, 0, 0, a->is_const ? -1 : a->slot,
                    b->is_const ? -1 : b->slot, a->value, b->value};
  if (a->is_const && b->is_const) {
    *out = ir_dot_const(ir_dot_apply(op, a->value, b->value));
    return 1;
  }
  return ir_dot_emit(body, step, out);
}

static int ir_dot_cast_value(IRDotBody *body, IRDotValue v, int bits,
                             int is_unsigned, IRDotValue *out) {
  IRDotStep step = {IR_DOT_OP_CAST, bits, is_unsigned, 0, v.slot, -1, 0, 0};
  *out = v;
  switch (v.kind) {
  case IR_DOT_DATA:
    if (bits == 64) {
      return 1;
    }
    if (v.is_const) {
      out->value = ir_dot_narrow(v.value, bits, is_unsigned);
      return 1;
    }
    return ir_dot_emit(body, step, out);
  case IR_DOT_INDEX:
  case IR_DOT_PRODUCT:
    return bits == 64;
  case IR_DOT_ELEM:
    return bits == 64 || (bits == 32 && !is_unsigned);
  case IR_DOT_ACC:
    return bits >= body->acc_bits;
  default:
    return 0;
  }
}

static int ir_dot_find(const IRDotBody *body, const char *name, int is_symbol) {
  for (int k = body->name_count; k-- > 0;) {
    if (body->name_is_symbol[k] == is_symbol &&
        strcmp(body->name[k], name) == 0) {
      return k;
    }
  }
  return -1;
}

static int ir_dot_bind(IRDotBody *body, const char *name, int is_symbol,
                       IRDotValue value) {
  int at = ir_dot_find(body, name, is_symbol);
  if (at < 0) {
    if (body->name_count >= IR_DOT_NAMES) {
      return 0;
    }
    at = body->name_count++;
    body->name[at] = name;
    body->name_is_symbol[at] = (unsigned char)is_symbol;
  }
  body->bound[at] = value;
  return 1;
}

static int ir_dot_read_base(IRDotBody *body, const char *name,
                            IRDotValue *out) {
  const char *type = ir_dot_symbol_type(body->function, name);
  if (!type || !ir_dot_type_is_pointer(type) ||
      (body->elem_size == 4 && strcmp(type, "int32*") != 0)) {
    return 0;
  }
  for (int s = 0; s < 2; s++) {
    if (!body->base[s]) {
      body->base[s] = name;
    }
    if (strcmp(body->base[s], name) == 0) {
      *out = ir_dot_index(s + 1, 0, 0);
      return 1;
    }
  }
  return 0;
}

static int ir_dot_read(IRDotBody *body, const IROperand *operand,
                       IRDotValue *out) {
  int at;
  if (operand->kind == IR_OPERAND_INT) {
    *out = ir_dot_const(operand->int_value);
    return 1;
  }
  if ((!ir_operand_is_temp(operand) && !ir_operand_is_symbol(operand)) ||
      !operand->name) {
    return 0;
  }
  at = ir_dot_find(body, operand->name, ir_operand_is_symbol(operand));
  if (at >= 0) {
    *out = body->bound[at];
    return 1;
  }
  if (ir_operand_is_temp(operand)) {
    return 0;
  }
  if (strcmp(operand->name, body->iv) == 0) {
    *out = body->iv_value;
    return 1;
  }
  if (strcmp(operand->name, body->acc) == 0) {
    *out = body->acc_value;
    return 1;
  }
  return ir_dot_read_base(body, operand->name, out);
}

static int ir_dot_is_base(const IRDotBody *body, const char *name) {
  return (body->base[0] && strcmp(body->base[0], name) == 0) ||
         (body->base[1] && strcmp(body->base[1], name) == 0);
}

static int ir_dot_write_local(IRDotBody *body, const char *name,
                              IRDotValue value) {
  const char *type = ir_function_local_declared_type(body->function, name);
  int bits = 0;
  int is_unsigned = 0;
  if (!type || ir_dot_is_base(body, name)) {
    return 0;
  }
  if (ir_dot_type_is_pointer(type)) {
    return value.kind == IR_DOT_INDEX && ir_dot_bind(body, name, 1, value);
  }
  return ir_dot_int_type(type, &bits, &is_unsigned) &&
         ir_dot_cast_value(body, value, bits, is_unsigned, &value) &&
         ir_dot_bind(body, name, 1, value);
}

static int ir_dot_write(IRDotBody *body, const IROperand *dest,
                        IRDotValue value) {
  if (ir_operand_is_temp(dest) && dest->name) {
    return ir_dot_bind(body, dest->name, 0, value);
  }
  if (!ir_operand_is_symbol(dest) || !dest->name) {
    return 0;
  }
  if (strcmp(dest->name, body->iv) == 0) {
    body->iv_value = value;
    return value.kind == IR_DOT_INDEX && value.side == 0;
  }
  if (strcmp(dest->name, body->acc) == 0) {
    body->acc_value = value;
    return value.kind == IR_DOT_ACC;
  }
  return ir_dot_write_local(body, dest->name, value);
}

static int ir_dot_index_op(IRDotOp op, IRDotValue a, const IRDotValue *b,
                           IRDotValue *out) {
  unsigned long long scale = (unsigned long long)a.scale;
  unsigned long long value = (unsigned long long)a.value;
  unsigned long long k = (unsigned long long)b->value;
  int by_const = b->kind == IR_DOT_DATA && b->is_const;
  *out = a;
  if (op == IR_DOT_OP_ADD && b->kind == IR_DOT_INDEX && !(a.side && b->side)) {
    out->side = a.side | b->side;
    out->scale = (long long)(scale + (unsigned long long)b->scale);
    out->value = (long long)(value + k);
    return 1;
  }
  if (!by_const) {
    return 0;
  }
  if (op == IR_DOT_OP_ADD || op == IR_DOT_OP_SUB) {
    out->value = (long long)(op == IR_DOT_OP_ADD ? value + k : value - k);
    return 1;
  }
  if (a.side != 0 || (op == IR_DOT_OP_SHL && k > 62)) {
    return 0;
  }
  if (op == IR_DOT_OP_SHL) {
    k = 1ULL << k;
  } else if (op != IR_DOT_OP_MUL) {
    return 0;
  }
  out->scale = (long long)(scale * k);
  out->value = (long long)(value * k);
  return 1;
}

static int ir_dot_acc_op(IRDotBody *body, IRDotOp op, IRDotValue a,
                         const IRDotValue *b, IRDotValue *out) {
  IRDotValue delta = a;
  IRDotValue sum;
  *out = a;
  if (op == IR_DOT_OP_ADD && b->kind == IR_DOT_PRODUCT) {
    out->scale = a.scale + 1;
    out->side = b->side;
    return 1;
  }
  if ((op != IR_DOT_OP_ADD && op != IR_DOT_OP_SUB) || b->kind != IR_DOT_DATA) {
    return 0;
  }
  delta.kind = IR_DOT_DATA;
  if (!ir_dot_data_op(body, op, &delta, b, &sum)) {
    return 0;
  }
  out->is_const = sum.is_const;
  out->slot = sum.slot;
  out->value = sum.value;
  return 1;
}

static int ir_dot_elem_op(IRDotOp op, IRDotValue a, const IRDotValue *b,
                          IRDotValue *out) {
  *out = a;
  out->kind = IR_DOT_PRODUCT;
  out->side = a.side | b->side;
  return op == IR_DOT_OP_MUL && b->kind == IR_DOT_ELEM;
}

static int ir_dot_binary(IRDotBody *body, const IRInstruction *ins,
                         IRDotValue *out) {
  IRDotOp op = ir_dot_binary_op(ins);
  IRDotValue a;
  IRDotValue b;
  if (op == IR_DOT_OP_UNKNOWN || !ir_dot_read(body, &ins->lhs, &a) ||
      !ir_dot_read(body, &ins->rhs, &b)) {
    return 0;
  }
  if ((op == IR_DOT_OP_ADD || op == IR_DOT_OP_MUL) && a.kind < b.kind) {
    IRDotValue swap = a;
    a = b;
    b = swap;
  }
  switch (a.kind) {
  case IR_DOT_DATA:
    return b.kind == IR_DOT_DATA && ir_dot_data_op(body, op, &a, &b, out);
  case IR_DOT_INDEX:
    return ir_dot_index_op(op, a, &b, out);
  case IR_DOT_ACC:
    return ir_dot_acc_op(body, op, a, &b, out);
  case IR_DOT_ELEM:
    return ir_dot_elem_op(op, a, &b, out);
  default:
    return 0;
  }
}

static int ir_dot_cast(IRDotBody *body, const IRInstruction *ins,
                       IRDotValue *out) {
  IRDotValue value;
  int bits = 0;
  int is_unsigned = 0;
  if (ins->is_float || !ins->text || !ir_dot_read(body, &ins->lhs, &value)) {
    return 0;
  }
  if (ir_dot_type_is_pointer(ins->text)) {
    *out = value;
    return value.kind == IR_DOT_INDEX;
  }
  return ir_dot_int_type(ins->text, &bits, &is_unsigned) &&
         ir_dot_cast_value(body, value, bits, is_unsigned, out);
}

static int ir_dot_load(IRDotBody *body, const IRInstruction *ins,
                       IRDotValue *out) {
  IRDotValue address;
  IRDotStep step = {IR_DOT_OP_LOAD, 8, ins->is_unsigned, 0, -1, -1, 0, 0};
  if (ins->is_float || ins->is_volatile || ins->rhs.kind != IR_OPERAND_INT ||
      ins->rhs.int_value != body->elem_size ||
      !ir_dot_read(body, &ins->lhs, &address) ||
      address.kind != IR_DOT_INDEX || address.side == 0 ||
      address.scale != body->elem_size || address.value != 0) {
    return 0;
  }
  if (body->elem_size == 4) {
    *out = address;
    out->kind = IR_DOT_ELEM;
    return !ins->is_unsigned;
  }
  step.side = address.side;
  return ir_dot_emit(body, step, out);
}

static int ir_dot_call_is_probe(const IRInstruction *ins) {
  return ins->text && (strncmp(ins->text, "mettle_profile_", 15) == 0 ||
                       strncmp(ins->text, "mettle_crash_trap", 17) == 0);
}

static int ir_dot_exec(IRDotBody *body, const IRInstruction *ins) {
  IRDotValue value;
  switch (ins->op) {
  case IR_OP_NOP:
  case IR_OP_DECLARE_LOCAL:
    return 1;
  case IR_OP_CALL:
    return ir_dot_call_is_probe(ins);
  case IR_OP_ASSIGN:
    return !ins->is_float && ir_dot_read(body, &ins->lhs, &value) &&
           ir_dot_write(body, &ins->dest, value);
  case IR_OP_CAST:
    return ir_dot_cast(body, ins, &value) &&
           ir_dot_write(body, &ins->dest, value);
  case IR_OP_BINARY:
    return ir_dot_binary(body, ins, &value) &&
           ir_dot_write(body, &ins->dest, value);
  case IR_OP_LOAD:
    return ir_dot_load(body, ins, &value) &&
           ir_dot_write(body, &ins->dest, value);
  default:
    return 0;
  }
}

static int ir_dot_compile(IRDotBody *body, size_t first, size_t last) {
  IRDotValue acc = {IR_DOT_ACC, 0, 1, -1, 0, 0};
  body->iv_value = ir_dot_index(0, 1, 0);
  body->acc_value = acc;
  for (size_t i = first; i < last; i++) {
    if (!ir_dot_exec(body, &body->function->instructions[i])) {
      return 0;
    }
  }
  return body->base[0] && body->iv_value.kind == IR_DOT_INDEX &&
         body->iv_value.side == 0 && body->iv_value.scale == 1 &&
         body->iv_value.value == 1 && body->acc_value.kind == IR_DOT_ACC;
}

static long long ir_dot_run(const IRDotBody *body, long long x, long long y) {
  long long slot[IR_DOT_STEPS];
  for (int s = 0; s < body->step_count; s++) {
    const IRDotStep *st = &body->step[s];
    long long a = st->a < 0 ? st->a_const : slot[st->a];
    long long b = st->b < 0 ? st->b_const : slot[st->b];
    if (st->op == IR_DOT_OP_LOAD) {
      slot[s] = ir_dot_narrow(st->side == 1 ? x : y, 8, st->is_unsigned);
    } else if (st->op == IR_DOT_OP_CAST) {
      slot[s] = ir_dot_narrow(a, st->bits, st->is_unsigned);
    } else {
      slot[s] = ir_dot_apply(st->op, a, b);
    }
  }
  return body->acc_value.is_const ? body->acc_value.value
                                  : slot[body->acc_value.slot];
}

static int ir_dot_modes_at(const IRDotBody *body, long long x, long long y,
                           int modes) {
  unsigned long long keep =
      body->acc_bits >= 64 ? ~0ULL : (1ULL << body->acc_bits) - 1;
  unsigned long long got = (unsigned long long)ir_dot_run(body, x, y);
  for (int mode = 0; mode < 4; mode++) {
    long long a = ir_dot_narrow(x, 8, mode & 1);
    long long b = ir_dot_narrow(y, 8, mode & 2);
    if (((got - (unsigned long long)(a * b)) & keep) != 0) {
      modes &= ~(1 << mode);
    }
  }
  return modes;
}

static const int IR_DOT_MODE_ORDER[4] = {0, 3, 1, 2};

static int ir_dot_byte_mode(const IRDotBody *body) {
  int single = body->base[1] == NULL;
  int modes = 15;
  if (body->acc_value.scale != 0) {
    return -1;
  }
  for (long long x = 0; x < 256 && modes; x++) {
    for (long long y = single ? x : 0; y < (single ? x + 1 : 256) && modes;
         y++) {
      modes = ir_dot_modes_at(body, x, y, modes);
    }
  }
  for (int k = 0; k < 4; k++) {
    if (modes & (1 << IR_DOT_MODE_ORDER[k])) {
      return IR_DOT_MODE_ORDER[k];
    }
  }
  return -1;
}

static int ir_dot_word_is_exact(const IRDotBody *body) {
  return body->acc_value.scale == 1 && body->acc_value.is_const &&
         body->acc_value.value == 0 &&
         body->acc_value.side == (body->base[1] ? 3 : 1);
}

static int ir_dot_symbol_written_in(const IRFunction *function, size_t lo,
                                    size_t hi, const char *name) {
  for (size_t i = lo; i <= hi; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ir_instruction_writes_destination(ins) &&
        ir_operand_is_symbol_named(&ins->dest, name)) {
      return 1;
    }
  }
  return 0;
}

static int ir_dot_bound_is_invariant(const IRFunction *function,
                                     const IROperand *bound, size_t header,
                                     size_t jump) {
  int bits = 0;
  int is_unsigned = 0;
  if (bound->kind == IR_OPERAND_INT) {
    return 1;
  }
  return ir_operand_is_symbol(bound) && bound->name &&
         ir_dot_int_type(ir_dot_symbol_type(function, bound->name), &bits,
                         &is_unsigned) &&
         !ir_dot_symbol_written_in(function, header, jump, bound->name);
}

static int ir_dot_find_back_edge(const IRFunction *function, size_t branch,
                                 const char *header_label,
                                 const char *exit_label, size_t *jump) {
  for (size_t i = branch + 1; i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_JUMP && ins->text &&
        strcmp(ins->text, header_label) == 0) {
      *jump = i;
      return 1;
    }
    if (ins->op == IR_OP_LABEL && ins->text &&
        strcmp(ins->text, exit_label) == 0) {
      return 0;
    }
  }
  return 0;
}

static int ir_dot_loop_frame(const IRFunction *function, size_t header,
                             IRDotLoop *loop) {
  const IRInstruction *label = &function->instructions[header];
  const IRInstruction *compare = NULL;
  const IRInstruction *branch = NULL;
  size_t compare_index = 0;
  if (label->op != IR_OP_LABEL || !ir_label_is_while_header(label->text) ||
      !ir_find_next_non_nop(function, header + 1, &compare_index) ||
      !ir_find_next_non_nop(function, compare_index + 1, &loop->branch)) {
    return 0;
  }
  compare = &function->instructions[compare_index];
  branch = &function->instructions[loop->branch];
  if (compare->op != IR_OP_BINARY || compare->is_float || !compare->text ||
      strcmp(compare->text, "<") != 0 || !ir_operand_is_temp(&compare->dest) ||
      !ir_operand_is_symbol(&compare->lhs) || !compare->lhs.name ||
      branch->op != IR_OP_BRANCH_ZERO || !branch->text ||
      !ir_operand_is_temp_named(&branch->lhs, compare->dest.name) ||
      !ir_dot_find_back_edge(function, loop->branch, label->text, branch->text,
                             &loop->jump)) {
    return 0;
  }
  loop->header = header;
  loop->iv = compare->lhs.name;
  loop->bound = &compare->rhs;
  return ir_fused_loop_exit_is_adjacent(function, loop->jump, branch->text);
}

static int ir_dot_match_loop(const IRFunction *function, size_t header,
                             IRDotLoop *loop) {
  return ir_dot_loop_frame(function, header, loop) &&
         ir_dot_bound_is_invariant(function, loop->bound, header, loop->jump) &&
         ir_iv_zero_at_header(function, header, loop->iv) &&
         !ir_symbol_live_after_loop(function, loop->jump + 1, loop->iv);
}

static int ir_dot_symbol_is_carried(const IRFunction *function,
                                    const IRDotLoop *loop, size_t at,
                                    const IROperand *operand) {
  int bits = 0;
  int is_unsigned = 0;
  return ir_operand_is_symbol(operand) && operand->name &&
         strcmp(operand->name, loop->iv) != 0 &&
         ir_dot_int_type(ir_dot_symbol_type(function, operand->name), &bits,
                         &is_unsigned) &&
         !ir_dot_symbol_written_in(function, loop->branch + 1, at - 1,
                                   operand->name) &&
         ir_dot_symbol_written_in(function, at, loop->jump, operand->name);
}

static const char *ir_dot_find_accumulator(const IRFunction *function,
                                           const IRDotLoop *loop) {
  for (size_t i = loop->branch + 1; i < loop->jump; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op != IR_OP_BINARY && ins->op != IR_OP_ASSIGN &&
        ins->op != IR_OP_CAST) {
      continue;
    }
    if (ir_dot_symbol_is_carried(function, loop, i, &ins->lhs)) {
      return ins->lhs.name;
    }
    if (ins->op == IR_OP_BINARY &&
        ir_dot_symbol_is_carried(function, loop, i, &ins->rhs)) {
      return ins->rhs.name;
    }
  }
  return NULL;
}

static int ir_dot_symbol_address_taken(const IRFunction *function,
                                       const char *name) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op == IR_OP_ADDRESS_OF &&
        ir_operand_is_symbol_named(&ins->lhs, name)) {
      return 1;
    }
  }
  return 0;
}

static int ir_dot_accumulator_fits(const IRFunction *function, const char *acc,
                                   int elem_size, int *bits,
                                   int *is_unsigned) {
  if (!ir_dot_int_type(ir_dot_symbol_type(function, acc), bits, is_unsigned) ||
      ir_dot_symbol_address_taken(function, acc)) {
    return 0;
  }
  return elem_size == 4 ? *bits == 64 && !*is_unsigned : *bits >= 32;
}

static int ir_dot_fill_arguments(IRInstruction *fused, const IROperand *bound,
                                 int mode, int acc_bits, int acc_unsigned) {
  size_t count = fused->op == IR_OP_SIMD_DOT_I8 ? 4 : 1;
  fused->arguments = calloc(count, sizeof(IROperand));
  if (!fused->arguments || !ir_operand_clone(bound, &fused->arguments[0])) {
    return 0;
  }
  fused->argument_count = count;
  if (count == 4) {
    fused->arguments[1] = ir_operand_int(mode);
    fused->arguments[2] = ir_operand_int(acc_bits);
    fused->arguments[3] = ir_operand_int(acc_unsigned);
    fused->is_unsigned = mode == 3;
  }
  return 1;
}

static int ir_dot_install(IRFunction *function, const IRDotLoop *loop,
                          IRInstruction *fused, int *changed) {
  IRInstruction *header = &function->instructions[loop->header];
  fused->location = header->location;
  if (!ir_fused_loop_claims_body(function, loop->header, loop->jump, fused)) {
    ir_instruction_destroy_storage(fused);
    return 1;
  }
  ir_instruction_destroy_storage(header);
  *header = *fused;
  for (size_t i = loop->header + 1; i <= loop->jump; i++) {
    ir_instruction_make_nop(&function->instructions[i]);
  }
  if (changed) {
    *changed = 1;
  }
  return 1;
}

static int ir_try_fuse_int_dot_at(IRFunction *function, size_t header,
                                  int elem_size, int *changed) {
  IRDotLoop loop = {0};
  IRDotBody body = {0};
  IRInstruction fused = {0};
  int acc_unsigned = 0;
  int mode = 0;
  if (!ir_dot_match_loop(function, header, &loop)) {
    return 1;
  }
  body.function = function;
  body.iv = loop.iv;
  body.elem_size = elem_size;
  body.acc = ir_dot_find_accumulator(function, &loop);
  if (!body.acc ||
      !ir_dot_accumulator_fits(function, body.acc, elem_size, &body.acc_bits,
                               &acc_unsigned) ||
      !ir_dot_compile(&body, loop.branch + 1, loop.jump)) {
    return 1;
  }
  mode = elem_size == 4 ? (ir_dot_word_is_exact(&body) ? 0 : -1)
                        : ir_dot_byte_mode(&body);
  if (mode < 0) {
    return 1;
  }
  if (!body.base[1]) {
    body.base[1] = body.base[0];
  }
  fused.op = elem_size == 4 ? IR_OP_SIMD_DOT_I32 : IR_OP_SIMD_DOT_I8;
  fused.dest = ir_operand_symbol(body.acc);
  fused.lhs = ir_operand_symbol(body.base[mode == 2 ? 1 : 0]);
  fused.rhs = ir_operand_symbol(body.base[mode == 2 ? 0 : 1]);
  if (!ir_dot_fill_arguments(&fused, loop.bound, mode == 2 ? 1 : mode,
                             body.acc_bits, acc_unsigned)) {
    ir_instruction_destroy_storage(&fused);
    return 0;
  }
  return ir_dot_install(function, &loop, &fused, changed);
}

static int ir_dot_multiplies_elements(const IRDotBody *body) {
  for (int s = 0; s < body->step_count; s++) {
    const IRDotStep *st = &body->step[s];
    if (st->op == IR_DOT_OP_MUL && st->a >= 0 && st->b >= 0) {
      return 1;
    }
  }
  return 0;
}

static int ir_dot_loaded_mode(const IRDotBody *body) {
  int mode = 0;
  for (int s = 0; s < body->step_count; s++) {
    const IRDotStep *st = &body->step[s];
    if (st->op == IR_DOT_OP_LOAD && st->is_unsigned) {
      mode |= st->side == 1 && body->base[1] ? 1 : st->side == 1 ? 3 : 2;
    }
  }
  return mode;
}

static int ir_dot_witness(const IRDotBody *body, int mode, long long *x,
                          long long *y) {
  int single = body->base[1] == NULL;
  for (*x = 0; *x < 256; (*x)++) {
    for (*y = single ? *x : 0; *y < (single ? *x + 1 : 256); (*y)++) {
      if (!(ir_dot_modes_at(body, *x, *y, 15) & (1 << mode))) {
        return 1;
      }
    }
  }
  return 0;
}

static void ir_dot_describe_witness(const IRDotBody *body, int acc_unsigned,
                                    char *reason, size_t reason_cap, char *fix,
                                    size_t fix_cap) {
  int mode = ir_dot_loaded_mode(body);
  long long x = 0;
  long long y = 0;
  char total[96];
  char a_name[96];
  char b_name[96];
  if (!ir_dot_witness(body, mode, &x, &y)) {
    return;
  }
  ir_display_name(body->acc, total, sizeof(total));
  ir_display_name(body->base[0], a_name, sizeof(a_name));
  ir_display_name(body->base[1] ? body->base[1] : body->base[0], b_name,
                  sizeof(b_name));
  snprintf(reason, reason_cap,
           "this multiplies 8-bit elements and adds to `%s`, and the amount "
           "added is the product of the two elements only for some values: "
           "with `%s[i]` at %lld and `%s[i]` at %lld the loop adds %lld and "
           "the product is %lld. The byte dot kernel takes a loop whose every "
           "iteration adds exactly that product",
           total, a_name, ir_dot_narrow(x, 8, mode & 1), b_name,
           ir_dot_narrow(y, 8, mode & 2),
           ir_dot_narrow(ir_dot_run(body, x, y), body->acc_bits, acc_unsigned),
           ir_dot_narrow(x, 8, mode & 1) * ir_dot_narrow(y, 8, mode & 2));
  snprintf(fix, fix_cap,
           "widen each element before multiplying, as in `%s = %s + "
           "(int32)%s[i] * (int32)%s[i]`, and apply a scale or an offset to "
           "the total after the loop",
           total, total, a_name, b_name);
}

static void ir_dot_describe_narrow_total(const IRFunction *function,
                                         const char *acc, char *reason,
                                         size_t reason_cap, char *fix,
                                         size_t fix_cap) {
  char total[96];
  ir_display_name(acc, total, sizeof(total));
  snprintf(reason, reason_cap,
           "this multiplies 8-bit elements and adds each product to `%s`, "
           "which is %s. The byte dot kernel keeps an exact 64-bit sum and "
           "wraps it to a 32-bit or 64-bit integer total",
           total, ir_dot_symbol_type(function, acc));
  snprintf(fix, fix_cap, "declare `%s` as int32, uint32, int64 or uint64",
           total);
}

int ir_dot_int_refusal(const IRFunction *function, size_t begin, size_t end,
                       char *reason, size_t reason_cap, char *fix,
                       size_t fix_cap) {
  IRDotLoop loop = {0};
  IRDotBody body = {0};
  int acc_unsigned = 0;
  size_t header = begin;
  while (header < end && !(function->instructions[header].op == IR_OP_LABEL &&
                           ir_label_is_while_header(
                               function->instructions[header].text))) {
    header++;
  }
  if (header >= end || !ir_dot_loop_frame(function, header, &loop)) {
    return 0;
  }
  body.function = function;
  body.iv = loop.iv;
  body.elem_size = 1;
  body.acc = ir_dot_find_accumulator(function, &loop);
  if (!body.acc || !ir_dot_int_type(ir_dot_symbol_type(function, body.acc),
                                    &body.acc_bits, &acc_unsigned)) {
    return 0;
  }
  if (!ir_dot_compile(&body, loop.branch + 1, loop.jump) ||
      !ir_dot_multiplies_elements(&body)) {
    return 0;
  }
  if (ir_dot_byte_mode(&body) < 0) {
    ir_dot_describe_witness(&body, acc_unsigned, reason, reason_cap, fix,
                            fix_cap);
    return reason[0] != '\0';
  }
  if (body.acc_bits >= 32) {
    return 0;
  }
  ir_dot_describe_narrow_total(function, body.acc, reason, reason_cap, fix,
                               fix_cap);
  return 1;
}

static int ir_simd_dot_int_pass(IRFunction *function, int elem_size,
                                int *changed) {
  if (!function) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op == IR_OP_LABEL &&
        ir_label_is_while_header(function->instructions[i].text) &&
        !ir_try_fuse_int_dot_at(function, i, elem_size, changed)) {
      return 0;
    }
  }
  return 1;
}

int ir_simd_dot_i32_pass(IRFunction *function, int *changed) {
  return ir_simd_dot_int_pass(function, 4, changed);
}

int ir_simd_dot_i8_pass(IRFunction *function, int *changed) {
  return ir_simd_dot_int_pass(function, 1, changed);
}
