#include "ir_scalarize.h"

#include "ir_lanes.h"
#include "optimizer/ir_optimize_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  IRFunction *out;
  const IRInstruction *at;
  int *fresh;
  int failed;
} Sc;

static IROperand sc_temp(Sc *sc) {
  char name[48];
  snprintf(name, sizeof(name), "__sc_t%d", (*sc->fresh)++);
  return ir_operand_temp(name);
}

static char *sc_label(Sc *sc) {
  char name[48];
  snprintf(name, sizeof(name), "__sc_l%d", (*sc->fresh)++);
  return mettle_strdup(name);
}

static void sc_push(Sc *sc, IRInstruction *ins) {
  ins->location = sc->at->location;
  if (!ir_function_append_instruction(sc->out, ins)) {
    sc->failed = 1;
  }
  ir_instruction_destroy_storage(ins);
}

static IRInstruction sc_blank(IROpcode op) {
  IRInstruction ins;
  memset(&ins, 0, sizeof(ins));
  ins.op = op;
  ins.dest = ir_operand_none();
  ins.lhs = ir_operand_none();
  ins.rhs = ir_operand_none();
  return ins;
}

static void sc_assign(Sc *sc, const IROperand *dest, const IROperand *value) {
  IRInstruction ins = sc_blank(IR_OP_ASSIGN);
  ins.dest = ir_operand_copy(dest);
  ins.lhs = ir_operand_copy(value);
  sc_push(sc, &ins);
}

static void sc_binary(Sc *sc, const IROperand *dest, const char *text,
                      const IROperand *a, const IROperand *b, int float_bits,
                      int is_unsigned) {
  IRInstruction ins = sc_blank(IR_OP_BINARY);
  ins.dest = ir_operand_copy(dest);
  ins.lhs = ir_operand_copy(a);
  ins.rhs = ir_operand_copy(b);
  ins.text = mettle_strdup(text);
  ins.is_float = float_bits != 0;
  ins.float_bits = float_bits;
  ins.is_unsigned = is_unsigned;
  sc_push(sc, &ins);
}

static IROperand sc_op(Sc *sc, const char *text, const IROperand *a,
                       const IROperand *b, int float_bits) {
  IROperand dest = sc_temp(sc);
  sc_binary(sc, &dest, text, a, b, float_bits, 0);
  return dest;
}

static IROperand sc_op_int(Sc *sc, const char *text, const IROperand *a,
                           long long b) {
  IROperand k = ir_operand_int(b);
  IROperand out = sc_op(sc, text, a, &k, 0);
  ir_operand_destroy(&k);
  return out;
}

static IROperand sc_cast(Sc *sc, const char *type, const IROperand *value) {
  IRInstruction ins = sc_blank(IR_OP_CAST);
  IROperand dest = sc_temp(sc);
  ins.dest = ir_operand_copy(&dest);
  ins.lhs = ir_operand_copy(value);
  ins.text = mettle_strdup(type);
  sc_push(sc, &ins);
  return dest;
}

static IROperand sc_address(Sc *sc, const IROperand *base, const IROperand *index,
                            long long scale) {
  IROperand offset = sc_op_int(sc, "*", index, scale);
  IROperand address = sc_op(sc, "+", base, &offset, 0);
  ir_operand_destroy(&offset);
  return address;
}

static IROperand sc_load(Sc *sc, const IROperand *address, long long size,
                         int float_bits, int is_unsigned) {
  IRInstruction ins = sc_blank(IR_OP_LOAD);
  IROperand dest = sc_temp(sc);
  ins.dest = ir_operand_copy(&dest);
  ins.lhs = ir_operand_copy(address);
  ins.rhs = ir_operand_int(size);
  ins.is_float = float_bits != 0;
  ins.float_bits = float_bits;
  ins.is_unsigned = is_unsigned;
  sc_push(sc, &ins);
  return dest;
}

static void sc_store(Sc *sc, const IROperand *address, const IROperand *value,
                     long long size, int float_bits) {
  IRInstruction ins = sc_blank(IR_OP_STORE);
  ins.dest = ir_operand_copy(address);
  ins.lhs = ir_operand_copy(value);
  ins.rhs = ir_operand_int(size);
  ins.is_float = float_bits != 0;
  ins.float_bits = float_bits;
  sc_push(sc, &ins);
}

static void sc_label_here(Sc *sc, const char *name) {
  IRInstruction ins = sc_blank(IR_OP_LABEL);
  ins.text = mettle_strdup(name);
  sc_push(sc, &ins);
}

static void sc_jump(Sc *sc, const char *name) {
  IRInstruction ins = sc_blank(IR_OP_JUMP);
  ins.text = mettle_strdup(name);
  sc_push(sc, &ins);
}

static void sc_branch_zero(Sc *sc, const IROperand *value, const char *name) {
  IRInstruction ins = sc_blank(IR_OP_BRANCH_ZERO);
  ins.lhs = ir_operand_copy(value);
  ins.text = mettle_strdup(name);
  sc_push(sc, &ins);
}

typedef struct {
  IROperand index;
  char *head;
  char *end;
} ScLoop;

static ScLoop sc_loop_from(Sc *sc, const IROperand *start, const IROperand *bound,
                           const char *compare) {
  ScLoop loop;
  loop.index = sc_temp(sc);
  loop.head = sc_label(sc);
  loop.end = sc_label(sc);
  sc_assign(sc, &loop.index, start);
  sc_label_here(sc, loop.head);
  IROperand test = sc_op(sc, compare, &loop.index, bound, 0);
  sc_branch_zero(sc, &test, loop.end);
  ir_operand_destroy(&test);
  return loop;
}

static ScLoop sc_loop(Sc *sc, const IROperand *bound) {
  IROperand zero = ir_operand_int(0);
  ScLoop loop = sc_loop_from(sc, &zero, bound, "<");
  ir_operand_destroy(&zero);
  return loop;
}

static void sc_loop_end(Sc *sc, ScLoop *loop, long long step) {
  IROperand k = ir_operand_int(step);
  sc_binary(sc, &loop->index, "+", &loop->index, &k, 0, 0);
  ir_operand_destroy(&k);
  sc_jump(sc, loop->head);
  sc_label_here(sc, loop->end);
  ir_operand_destroy(&loop->index);
  free(loop->head);
  free(loop->end);
}

static void sc_if(Sc *sc, const IROperand *truth, const IROperand *dest,
                  const IROperand *value) {
  char *skip = sc_label(sc);
  sc_branch_zero(sc, truth, skip);
  sc_assign(sc, dest, value);
  sc_label_here(sc, skip);
  free(skip);
}

static void sc_call_float(Sc *sc, const IROperand *dest, const char *name,
                          const IROperand *value) {
  IRInstruction ins = sc_blank(IR_OP_CALL);
  IROperand argument[1];
  argument[0] = ir_operand_copy(value);
  ins.dest = ir_operand_copy(dest);
  ins.text = mettle_strdup(name);
  ins.is_float = 1;
  ins.float_bits = 64;
  ins.arguments = malloc(sizeof(IROperand));
  ins.argument_count = 1;
  ins.arguments[0] = argument[0];
  sc_push(sc, &ins);
}

static void sc_select(Sc *sc, const IRInstruction *insn) {
  char *otherwise = sc_label(sc);
  char *end = sc_label(sc);
  if (insn->text && insn->argument_count > 1) {
    IROperand truth = sc_temp(sc);
    sc_binary(sc, &truth, insn->text, &insn->lhs, &insn->arguments[1], 0,
              insn->is_unsigned);
    sc_branch_zero(sc, &truth, otherwise);
    ir_operand_destroy(&truth);
  } else {
    sc_branch_zero(sc, &insn->lhs, otherwise);
  }
  sc_assign(sc, &insn->dest, &insn->rhs);
  sc_jump(sc, end);
  sc_label_here(sc, otherwise);
  if (insn->argument_count > 0) {
    sc_assign(sc, &insn->dest, &insn->arguments[0]);
  }
  sc_label_here(sc, end);
  free(otherwise);
  free(end);
}

static void sc_float_sum(Sc *sc, const IRInstruction *insn) {
  int bits = insn->op == IR_OP_SIMD_SUM_F32 ? 32 : 64;
  long long size = bits / 8;
  ScLoop loop = sc_loop(sc, &insn->rhs);
  IROperand address = sc_address(sc, &insn->lhs, &loop.index, size);
  IROperand value = sc_load(sc, &address, size, bits, 0);
  sc_binary(sc, &insn->dest, "+", &insn->dest, &value, bits, 0);
  ir_operand_destroy(&address);
  ir_operand_destroy(&value);
  sc_loop_end(sc, &loop, 1);
}

static IROperand sc_dot_operand(Sc *sc, const IROperand *base,
                                const IROperand *index, int widen, int bits) {
  long long size = widen ? 4 : bits / 8;
  IROperand at = sc_address(sc, base, index, size);
  IROperand value = sc_load(sc, &at, size, widen ? 32 : bits, 0);
  ir_operand_destroy(&at);
  if (!widen) {
    return value;
  }
  IROperand wide = sc_cast(sc, "float64", &value);
  ir_operand_destroy(&value);
  return wide;
}

static void sc_float_dot(Sc *sc, const IRInstruction *insn) {
  int bits = insn->op == IR_OP_SIMD_DOT_F32 ? 32 : 64;
  int widen = insn->argument_count > 1 &&
              insn->arguments[1].kind == IR_OPERAND_INT &&
              insn->arguments[1].int_value == 1;
  ScLoop loop = sc_loop(sc, &insn->arguments[0]);
  IROperand a = sc_dot_operand(sc, &insn->lhs, &loop.index, widen, bits);
  IROperand b = sc_dot_operand(sc, &insn->rhs, &loop.index, widen, bits);
  IROperand product = sc_op(sc, "*", &a, &b, bits);
  sc_binary(sc, &insn->dest, "+", &insn->dest, &product, bits, 0);
  ir_operand_destroy(&a);
  ir_operand_destroy(&b);
  ir_operand_destroy(&product);
  sc_loop_end(sc, &loop, 1);
}

static void sc_float_affine(Sc *sc, const IRInstruction *insn) {
  int bits = insn->op == IR_OP_SIMD_AFFINE_MAP_F32 ? 32 : 64;
  long long size = bits / 8;
  ScLoop loop = sc_loop(sc, &insn->arguments[0]);
  IROperand x_at = sc_address(sc, &insn->lhs, &loop.index, size);
  IROperand y_at = sc_address(sc, &insn->rhs, &loop.index, size);
  IROperand x = sc_load(sc, &x_at, size, bits, 0);
  IROperand y = sc_load(sc, &y_at, size, bits, 0);
  IROperand ax = sc_op(sc, "*", &insn->arguments[1], &x, bits);
  IROperand by = sc_op(sc, "*", &insn->arguments[2], &y, bits);
  IROperand sum = sc_op(sc, "+", &ax, &by, bits);
  IROperand result = sc_op(sc, "+", &sum, &insn->arguments[3], bits);
  sc_store(sc, &y_at, &result, size, bits);
  ir_operand_destroy(&x_at);
  ir_operand_destroy(&y_at);
  ir_operand_destroy(&x);
  ir_operand_destroy(&y);
  ir_operand_destroy(&ax);
  ir_operand_destroy(&by);
  ir_operand_destroy(&sum);
  ir_operand_destroy(&result);
  sc_loop_end(sc, &loop, 1);
}

static void sc_float_map(Sc *sc, const IRInstruction *insn) {
  ScLoop loop = sc_loop(sc, &insn->arguments[0]);
  IROperand out_at = sc_address(sc, &insn->dest, &loop.index, 4);
  IROperand result = sc_temp(sc);
  if (insn->op == IR_OP_SIMD_EXP_F32) {
    IROperand v = sc_load(sc, &out_at, 4, 32, 0);
    IROperand wide = sc_cast(sc, "float64", &v);
    IROperand e = sc_temp(sc);
    sc_call_float(sc, &e, "exp", &wide);
    IROperand narrow = sc_cast(sc, "float32", &e);
    sc_assign(sc, &result, &narrow);
    ir_operand_destroy(&v);
    ir_operand_destroy(&wide);
    ir_operand_destroy(&e);
    ir_operand_destroy(&narrow);
  } else {
    IROperand g_at = sc_address(sc, &insn->lhs, &loop.index, 4);
    IROperand g = sc_load(sc, &g_at, 4, 32, 0);
    IROperand wide = sc_cast(sc, "float64", &g);
    IROperand zero = ir_operand_float_sized(0.0, 64);
    IROperand minus = sc_op(sc, "-", &zero, &wide, 64);
    IROperand e = sc_temp(sc);
    sc_call_float(sc, &e, "exp", &minus);
    IROperand one = ir_operand_float_sized(1.0, 64);
    IROperand bottom = sc_op(sc, "+", &one, &e, 64);
    IROperand quotient = sc_op(sc, "/", &wide, &bottom, 64);
    IROperand narrow = sc_cast(sc, "float32", &quotient);
    if (insn->rhs.kind == IR_OPERAND_NONE ||
        (insn->rhs.kind == IR_OPERAND_STRING &&
         (!insn->rhs.name || !insn->rhs.name[0]))) {
      sc_assign(sc, &result, &narrow);
    } else {
      IROperand u_at = sc_address(sc, &insn->rhs, &loop.index, 4);
      IROperand u = sc_load(sc, &u_at, 4, 32, 0);
      sc_binary(sc, &result, "*", &narrow, &u, 32, 0);
      ir_operand_destroy(&u_at);
      ir_operand_destroy(&u);
    }
    ir_operand_destroy(&g_at);
    ir_operand_destroy(&g);
    ir_operand_destroy(&wide);
    ir_operand_destroy(&zero);
    ir_operand_destroy(&minus);
    ir_operand_destroy(&e);
    ir_operand_destroy(&one);
    ir_operand_destroy(&bottom);
    ir_operand_destroy(&quotient);
    ir_operand_destroy(&narrow);
  }
  sc_store(sc, &out_at, &result, 4, 32);
  ir_operand_destroy(&out_at);
  ir_operand_destroy(&result);
  sc_loop_end(sc, &loop, 1);
}

static void sc_int_sum(Sc *sc, const IRInstruction *insn) {
  int is_u8 = insn->op == IR_OP_SIMD_SUM_U8;
  ScLoop loop = sc_loop(sc, &insn->rhs);
  IROperand address = sc_address(sc, &insn->lhs, &loop.index, is_u8 ? 1 : 4);
  IROperand value = sc_load(sc, &address, is_u8 ? 1 : 4, 0, is_u8);
  sc_binary(sc, &insn->dest, "+", &insn->dest, &value, 0, 0);
  ir_operand_destroy(&address);
  ir_operand_destroy(&value);
  sc_loop_end(sc, &loop, 1);
}

static void sc_int_dot(Sc *sc, const IRInstruction *insn) {
  int is_i8 = insn->op == IR_OP_SIMD_DOT_I8;
  long long size = is_i8 ? 1 : 4;
  ScLoop loop = sc_loop(sc, &insn->arguments[0]);
  IROperand a_at = sc_address(sc, &insn->lhs, &loop.index, size);
  IROperand b_at = sc_address(sc, &insn->rhs, &loop.index, size);
  IROperand a = sc_load(sc, &a_at, size, 0, is_i8 && insn->is_unsigned);
  IROperand b = sc_load(sc, &b_at, size, 0, is_i8 && insn->is_unsigned);
  IROperand product = sc_op(sc, "*", &a, &b, 0);
  IROperand sum = sc_op(sc, "+", &insn->dest, &product, 0);
  if (is_i8) {
    IROperand wrapped = sc_cast(sc, "int32", &sum);
    sc_assign(sc, &insn->dest, &wrapped);
    ir_operand_destroy(&wrapped);
  } else {
    sc_assign(sc, &insn->dest, &sum);
  }
  ir_operand_destroy(&a_at);
  ir_operand_destroy(&b_at);
  ir_operand_destroy(&a);
  ir_operand_destroy(&b);
  ir_operand_destroy(&product);
  ir_operand_destroy(&sum);
  sc_loop_end(sc, &loop, 1);
}

static void sc_prefix_sum(Sc *sc, const IRInstruction *insn) {
  ScLoop loop = sc_loop(sc, &insn->arguments[0]);
  IROperand from = sc_address(sc, &insn->lhs, &loop.index, 4);
  IROperand to = sc_address(sc, &insn->rhs, &loop.index, 4);
  IROperand value = sc_load(sc, &from, 4, 0, 0);
  sc_binary(sc, &insn->dest, "+", &insn->dest, &value, 0, 0);
  sc_store(sc, &to, &insn->dest, 4, 0);
  ir_operand_destroy(&from);
  ir_operand_destroy(&to);
  ir_operand_destroy(&value);
  sc_loop_end(sc, &loop, 1);
}

static void sc_int_map(Sc *sc, const IRInstruction *insn) {
  ScLoop loop = sc_loop(sc, &insn->arguments[0]);
  IROperand source_index = sc_temp(sc);
  if (insn->op == IR_OP_SIMD_REVERSE_COPY_I32) {
    IROperand last = sc_op_int(sc, "-", &insn->arguments[0], 1);
    sc_binary(sc, &source_index, "-", &last, &loop.index, 0, 0);
    ir_operand_destroy(&last);
  } else {
    sc_assign(sc, &source_index, &loop.index);
  }
  IROperand from = sc_address(sc, &insn->lhs, &source_index, 4);
  IROperand to = sc_address(sc, &insn->rhs, &loop.index, 4);
  IROperand value = sc_load(sc, &from, 4, 0, 0);
  IROperand result = sc_temp(sc);
  if (insn->op == IR_OP_SIMD_SCALE_I32) {
    IROperand product = sc_op(sc, "*", &value, &insn->arguments[1], 0);
    IROperand sum = sc_op(sc, "+", &product, &insn->arguments[2], 0);
    IROperand wrapped = sc_cast(sc, "int32", &sum);
    sc_assign(sc, &result, &wrapped);
    ir_operand_destroy(&product);
    ir_operand_destroy(&sum);
    ir_operand_destroy(&wrapped);
  } else if (insn->op == IR_OP_SIMD_CLAMP_I32) {
    sc_assign(sc, &result, &value);
    IROperand low = sc_op(sc, "<", &value, &insn->arguments[1], 0);
    sc_if(sc, &low, &result, &insn->arguments[1]);
    IROperand high = sc_op(sc, ">", &value, &insn->arguments[2], 0);
    sc_if(sc, &high, &result, &insn->arguments[2]);
    ir_operand_destroy(&low);
    ir_operand_destroy(&high);
  } else {
    sc_assign(sc, &result, &value);
  }
  sc_store(sc, &to, &result, 4, 0);
  sc_binary(sc, &insn->dest, "+", &insn->dest, &result, 0, 0);
  ir_operand_destroy(&source_index);
  ir_operand_destroy(&from);
  ir_operand_destroy(&to);
  ir_operand_destroy(&value);
  ir_operand_destroy(&result);
  sc_loop_end(sc, &loop, 1);
}

static void sc_lower_bound(Sc *sc, const IRInstruction *insn) {
  IROperand high = sc_temp(sc);
  char *head = sc_label(sc);
  char *end = sc_label(sc);
  char *otherwise = sc_label(sc);
  char *next = sc_label(sc);
  sc_assign(sc, &high, &insn->rhs);
  sc_label_here(sc, head);
  IROperand test = sc_op(sc, "<", &insn->dest, &high, 0);
  sc_branch_zero(sc, &test, end);
  IROperand gap = sc_op(sc, "-", &high, &insn->dest, 0);
  IROperand half = sc_op_int(sc, "/", &gap, 2);
  IROperand mid = sc_op(sc, "+", &insn->dest, &half, 0);
  IROperand at = sc_address(sc, &insn->lhs, &mid, 4);
  IROperand value = sc_load(sc, &at, 4, 0, 0);
  IROperand below = sc_op(sc, "<", &value, &insn->arguments[0], 0);
  sc_branch_zero(sc, &below, otherwise);
  IROperand after = sc_op_int(sc, "+", &mid, 1);
  sc_assign(sc, &insn->dest, &after);
  sc_jump(sc, next);
  sc_label_here(sc, otherwise);
  sc_assign(sc, &high, &mid);
  sc_label_here(sc, next);
  sc_jump(sc, head);
  sc_label_here(sc, end);
  ir_operand_destroy(&high);
  ir_operand_destroy(&test);
  ir_operand_destroy(&gap);
  ir_operand_destroy(&half);
  ir_operand_destroy(&mid);
  ir_operand_destroy(&at);
  ir_operand_destroy(&value);
  ir_operand_destroy(&below);
  ir_operand_destroy(&after);
  free(head);
  free(end);
  free(otherwise);
  free(next);
}

static void sc_minmax(Sc *sc, const IRInstruction *insn) {
  IROperand one = ir_operand_int(1);
  ScLoop loop = sc_loop_from(sc, &one, &insn->rhs, "<");
  ir_operand_destroy(&one);
  IROperand at = sc_address(sc, &insn->lhs, &loop.index, 4);
  IROperand value = sc_load(sc, &at, 4, 0, 0);
  IROperand low = sc_op(sc, "<", &value, &insn->dest, 0);
  sc_if(sc, &low, &insn->dest, &value);
  IROperand high = sc_op(sc, ">", &value, &insn->arguments[0], 0);
  sc_if(sc, &high, &insn->arguments[0], &value);
  ir_operand_destroy(&at);
  ir_operand_destroy(&value);
  ir_operand_destroy(&low);
  ir_operand_destroy(&high);
  sc_loop_end(sc, &loop, 1);
}

static void sc_copy(Sc *sc, const IRInstruction *insn) {
  long long elem = insn->arguments[0].int_value;
  ScLoop loop = sc_loop(sc, &insn->rhs);
  IROperand from = sc_address(sc, &insn->lhs, &loop.index, elem);
  IROperand to = sc_address(sc, &insn->dest, &loop.index, elem);
  IROperand value = sc_load(sc, &from, elem, 0, 1);
  sc_store(sc, &to, &value, elem, 0);
  ir_operand_destroy(&from);
  ir_operand_destroy(&to);
  ir_operand_destroy(&value);
  sc_loop_end(sc, &loop, 1);
}

static void sc_memcpy_inline(Sc *sc, const IRInstruction *insn) {
  if (insn->rhs.kind == IR_OPERAND_INT &&
      (insn->rhs.int_value == 1 || insn->rhs.int_value == 2 ||
       insn->rhs.int_value == 4 || insn->rhs.int_value == 8)) {
    IROperand value = sc_load(sc, &insn->lhs, insn->rhs.int_value, 0, 1);
    sc_store(sc, &insn->dest, &value, insn->rhs.int_value, 0);
    ir_operand_destroy(&value);
    return;
  }
  IRInstruction ins = sc_blank(IR_OP_STORE);
  ins.dest = ir_operand_copy(&insn->dest);
  ins.lhs = ir_operand_copy(&insn->lhs);
  ins.rhs = ir_operand_copy(&insn->rhs);
  sc_push(sc, &ins);
}

static int sc_fill(Sc *sc, const IRInstruction *insn) {
  long long elem = insn->arguments[0].int_value;
  long long mode = insn->arguments[1].int_value;
  const IROperand *value = &insn->arguments[2];
  int float_bits = value->kind == IR_OPERAND_FLOAT ? (int)(elem * 8) : 0;
  if (value->kind == IR_OPERAND_TEMP || value->kind == IR_OPERAND_SYMBOL) {
    float_bits = insn->is_float ? (int)(elem * 8) : 0;
  }
  if (mode == 0) {
    IROperand start = insn->argument_count > 3 ? ir_operand_copy(&insn->arguments[3])
                                               : ir_operand_int(0);
    ScLoop loop = sc_loop_from(sc, &start, &insn->rhs, "<");
    IROperand index = insn->argument_count > 4
                          ? sc_op(sc, "+", &loop.index, &insn->arguments[4], 0)
                          : ir_operand_copy(&loop.index);
    IROperand at = sc_address(sc, &insn->lhs, &index, elem);
    sc_store(sc, &at, value, elem, float_bits);
    ir_operand_destroy(&index);
    ir_operand_destroy(&at);
    ir_operand_destroy(&start);
    if (insn->dest.kind == IR_OPERAND_SYMBOL) {
      sc_assign(sc, &insn->dest, &loop.index);
      IROperand keep = ir_operand_copy(&loop.index);
      sc_loop_end(sc, &loop, 1);
      sc_assign(sc, &insn->dest, &keep);
      ir_operand_destroy(&keep);
    } else {
      sc_loop_end(sc, &loop, 1);
    }
    return 1;
  }
  if (mode == 1) {
    IROperand cursor = sc_temp(sc);
    char *head = sc_label(sc);
    char *end = sc_label(sc);
    sc_assign(sc, &cursor, &insn->lhs);
    sc_label_here(sc, head);
    IROperand test = sc_op(sc, "<", &cursor, &insn->rhs, 0);
    sc_branch_zero(sc, &test, end);
    sc_store(sc, &cursor, value, elem, float_bits);
    IROperand next = sc_op_int(sc, "+", &cursor, elem);
    sc_assign(sc, &cursor, &next);
    sc_jump(sc, head);
    sc_label_here(sc, end);
    ir_operand_destroy(&cursor);
    ir_operand_destroy(&test);
    ir_operand_destroy(&next);
    free(head);
    free(end);
    return 1;
  }
  if (mode == 2) {
    IROperand start = insn->argument_count > 3 ? ir_operand_copy(&insn->arguments[3])
                                               : ir_operand_int(0);
    ScLoop loop = sc_loop_from(sc, &start, &insn->rhs, "<");
    IROperand at = sc_op(sc, "+", &insn->lhs, &loop.index, 0);
    sc_store(sc, &at, value, elem, float_bits);
    ir_operand_destroy(&at);
    ir_operand_destroy(&start);
    IROperand keep = ir_operand_copy(&loop.index);
    sc_loop_end(sc, &loop, elem);
    if (insn->dest.kind == IR_OPERAND_SYMBOL) {
      sc_assign(sc, &insn->dest, &keep);
    }
    ir_operand_destroy(&keep);
    return 1;
  }
  return 0;
}

static void sc_byte_map(Sc *sc, const IRInstruction *insn) {
  static const char *const OPS[] = {"+", "-", "*", "^", "&", "|"};
  ScLoop loop = sc_loop(sc, &insn->rhs);
  IROperand at = sc_address(sc, &insn->lhs, &loop.index, 1);
  IROperand b = sc_load(sc, &at, 1, 0, 1);
  IROperand current = ir_operand_copy(&b);
  for (size_t s = 0; s + 1 < insn->argument_count; s += 2) {
    long long code = insn->arguments[s].int_value;
    long long k = insn->arguments[s + 1].int_value & 0xFF;
    IROperand step = sc_op_int(sc, OPS[code], &current, k);
    IROperand masked = sc_op_int(sc, "&", &step, 0xFF);
    ir_operand_destroy(&current);
    ir_operand_destroy(&step);
    current = masked;
  }
  sc_store(sc, &at, &current, 1, 0);
  ir_operand_destroy(&at);
  ir_operand_destroy(&b);
  ir_operand_destroy(&current);
  sc_loop_end(sc, &loop, 1);
}

static void sc_rotate_add(Sc *sc, const IRInstruction *insn) {
  IROperand next = sc_op(sc, "+", &insn->lhs, &insn->rhs, 0);
  sc_assign(sc, &insn->lhs, &insn->rhs);
  sc_assign(sc, &insn->rhs, &next);
  sc_assign(sc, &insn->dest, &next);
  ir_operand_destroy(&next);
}

static void sc_lcg(Sc *sc, const IRInstruction *insn) {
  ScLoop loop = sc_loop(sc, &insn->lhs);
  IROperand product =
      sc_op_int(sc, "*", &insn->rhs, insn->arguments[0].int_value);
  IROperand sum = sc_op_int(sc, "+", &product, insn->arguments[1].int_value);
  IROperand wrapped = sc_op_int(sc, "&", &sum, 0xFFFFFFFFLL);
  sc_assign(sc, &insn->rhs, &wrapped);
  IROperand masked = sc_op_int(sc, "&", &wrapped, insn->arguments[2].int_value);
  sc_binary(sc, &insn->dest, "+", &insn->dest, &masked, 0, 0);
  ir_operand_destroy(&product);
  ir_operand_destroy(&sum);
  ir_operand_destroy(&wrapped);
  ir_operand_destroy(&masked);
  sc_loop_end(sc, &loop, 1);
}

static void sc_lanes(Sc *sc, const IRInstruction *insn) {
  LaneLayout layout;
  if (!ir_lanes_layout(insn, &layout)) {
    return;
  }
  for (int r = 0; r < layout.n_reduce; r++) {
    int fsum = insn->arguments[layout.kinds_at + r].int_value ==
               LANE_REDUCE_FSUM;
    IROperand identity = ir_operand_int(fsum ? 0x80000000LL : 0);
    sc_store(sc, &insn->arguments[layout.partials_at + r], &identity, 4, 0);
    ir_operand_destroy(&identity);
  }
}

static void sc_i2f_reduce(Sc *sc, const IRInstruction *insn) {
  ScLoop loop = sc_loop(sc, &insn->arguments[0]);
  IROperand x = sc_cast(sc, "float64", &loop.index);
  for (size_t s = 1; s + 1 < insn->argument_count; s += 2) {
    long long code = insn->arguments[s].int_value;
    const IROperand *k = &insn->arguments[s + 1];
    IROperand next = code == 0   ? sc_op(sc, "*", &x, k, 64)
                     : code == 1 ? sc_op(sc, "+", &x, k, 64)
                     : code == 2 ? sc_op(sc, "-", &x, k, 64)
                     : code == 3 ? sc_op(sc, "-", k, &x, 64)
                                 : sc_op(sc, "/", &x, k, 64);
    ir_operand_destroy(&x);
    x = next;
  }
  IROperand whole = sc_cast(sc, "int64", &x);
  sc_binary(sc, &insn->dest, "+", &insn->dest, &whole, 0, 0);
  ir_operand_destroy(&x);
  ir_operand_destroy(&whole);
  sc_loop_end(sc, &loop, 1);
}

static void sc_slp_mac(Sc *sc, const IRInstruction *insn) {
  int is_i8 = insn->op == IR_OP_SIMD_SLP_MAC_I8;
  long long elem = is_i8 ? 1 : 4;
  long long lanes = insn->arguments[0].int_value;
  for (long long j = 0; j < lanes; j++) {
    IROperand sum = sc_temp(sc);
    IROperand zero = ir_operand_int(0);
    sc_assign(sc, &sum, &zero);
    ir_operand_destroy(&zero);
    ScLoop loop = sc_loop(sc, &insn->arguments[1]);
    IROperand a_index = sc_op(sc, "+", &loop.index, &insn->arguments[2], 0);
    IROperand b_scaled = sc_op(sc, "*", &loop.index, &insn->arguments[4], 0);
    IROperand b_base = sc_op(sc, "+", &b_scaled, &insn->arguments[3], 0);
    IROperand b_index = sc_op_int(sc, "+", &b_base, j);
    IROperand a_at = sc_address(sc, &insn->lhs, &a_index, elem);
    IROperand b_at = sc_address(sc, &insn->rhs, &b_index, elem);
    IROperand a = sc_load(sc, &a_at, elem, 0, is_i8);
    IROperand b = sc_load(sc, &b_at, elem, 0, is_i8);
    IROperand product = sc_op(sc, "*", &a, &b, 0);
    sc_binary(sc, &sum, "+", &sum, &product, 0, 0);
    ir_operand_destroy(&a_index);
    ir_operand_destroy(&b_scaled);
    ir_operand_destroy(&b_base);
    ir_operand_destroy(&b_index);
    ir_operand_destroy(&a_at);
    ir_operand_destroy(&b_at);
    ir_operand_destroy(&a);
    ir_operand_destroy(&b);
    ir_operand_destroy(&product);
    sc_loop_end(sc, &loop, 1);
    IROperand out_index = sc_op_int(sc, "+", &insn->arguments[5], j);
    IROperand out_at = sc_address(sc, &insn->dest, &out_index, 4);
    sc_store(sc, &out_at, &sum, 4, 0);
    ir_operand_destroy(&out_index);
    ir_operand_destroy(&out_at);
    ir_operand_destroy(&sum);
  }
}

static void sc_insertion_sort(Sc *sc, const IRInstruction *insn) {
  IROperand one = ir_operand_int(1);
  ScLoop outer = sc_loop_from(sc, &one, &insn->rhs, "<");
  ir_operand_destroy(&one);
  IROperand key_at = sc_address(sc, &insn->dest, &outer.index, 4);
  IROperand key = sc_load(sc, &key_at, 4, 0, 0);
  IROperand j = sc_temp(sc);
  IROperand start = sc_op_int(sc, "-", &outer.index, 1);
  sc_assign(sc, &j, &start);
  char *head = sc_label(sc);
  char *end = sc_label(sc);
  sc_label_here(sc, head);
  IROperand zero = ir_operand_int(0);
  IROperand alive = sc_op(sc, ">=", &j, &zero, 0);
  sc_branch_zero(sc, &alive, end);
  IROperand v_at = sc_address(sc, &insn->dest, &j, 4);
  IROperand v = sc_load(sc, &v_at, 4, 0, 0);
  IROperand above = sc_op(sc, ">", &v, &key, 0);
  sc_branch_zero(sc, &above, end);
  IROperand j1 = sc_op_int(sc, "+", &j, 1);
  IROperand to = sc_address(sc, &insn->dest, &j1, 4);
  sc_store(sc, &to, &v, 4, 0);
  IROperand back = sc_op_int(sc, "-", &j, 1);
  sc_assign(sc, &j, &back);
  sc_jump(sc, head);
  sc_label_here(sc, end);
  IROperand j2 = sc_op_int(sc, "+", &j, 1);
  IROperand last = sc_address(sc, &insn->dest, &j2, 4);
  sc_store(sc, &last, &key, 4, 0);
  ir_operand_destroy(&key_at);
  ir_operand_destroy(&key);
  ir_operand_destroy(&j);
  ir_operand_destroy(&start);
  ir_operand_destroy(&zero);
  ir_operand_destroy(&alive);
  ir_operand_destroy(&v_at);
  ir_operand_destroy(&v);
  ir_operand_destroy(&above);
  ir_operand_destroy(&j1);
  ir_operand_destroy(&to);
  ir_operand_destroy(&back);
  ir_operand_destroy(&j2);
  ir_operand_destroy(&last);
  free(head);
  free(end);
  sc_loop_end(sc, &outer, 1);
}

static void sc_count_word_starts(Sc *sc, const IRInstruction *insn) {
  IROperand in_word = sc_temp(sc);
  IROperand zero = ir_operand_int(0);
  IROperand one = ir_operand_int(1);
  sc_assign(sc, &in_word, &zero);
  ScLoop loop = sc_loop(sc, &insn->rhs);
  IROperand at = sc_address(sc, &insn->lhs, &loop.index, 1);
  IROperand byte = sc_load(sc, &at, 1, 0, 1);
  char *blank = sc_label(sc);
  char *word = sc_label(sc);
  char *next = sc_label(sc);
  static const long long SPACES[] = {0x20, 0x09, 0x0A, 0x0D};
  for (size_t s = 0; s < sizeof(SPACES) / sizeof(SPACES[0]); s++) {
    IROperand k = ir_operand_int(SPACES[s]);
    IROperand differs = sc_op(sc, "!=", &byte, &k, 0);
    sc_branch_zero(sc, &differs, blank);
    ir_operand_destroy(&k);
    ir_operand_destroy(&differs);
  }
  sc_jump(sc, word);
  sc_label_here(sc, blank);
  sc_assign(sc, &in_word, &zero);
  sc_jump(sc, next);
  sc_label_here(sc, word);
  IROperand fresh = sc_op(sc, "==", &in_word, &zero, 0);
  IROperand bumped = sc_op(sc, "+", &insn->dest, &one, 0);
  sc_if(sc, &fresh, &insn->dest, &bumped);
  sc_assign(sc, &in_word, &one);
  sc_label_here(sc, next);
  ir_operand_destroy(&at);
  ir_operand_destroy(&byte);
  ir_operand_destroy(&fresh);
  ir_operand_destroy(&bumped);
  free(blank);
  free(word);
  free(next);
  sc_loop_end(sc, &loop, 1);
  ir_operand_destroy(&in_word);
  ir_operand_destroy(&zero);
  ir_operand_destroy(&one);
}

static void sc_find(Sc *sc, const IRInstruction *insn) {
  static const char *const PREDICATES[] = {"==", "!=", "<", ">", "<=", ">="};
  long long pred = insn->arguments[0].int_value;
  long long elem_kind = insn->arguments[1].int_value;
  long long rhs_kind = insn->arguments[2].int_value;
  long long size = elem_kind == 0 ? 4 : 1;
  IROperand first = insn->argument_count > 4 ? ir_operand_copy(&insn->arguments[4])
                                             : ir_operand_int(0);
  IROperand hit = sc_temp(sc);
  sc_assign(sc, &hit, &insn->lhs);
  IROperand beyond = sc_op(sc, ">", &first, &insn->lhs, 0);
  sc_if(sc, &beyond, &hit, &first);
  char *found = sc_label(sc);
  ScLoop loop = sc_loop_from(sc, &first, &insn->lhs, "<");
  IROperand at = sc_address(sc, &insn->rhs, &loop.index, size);
  IROperand a = sc_load(sc, &at, size, 0, size == 1);
  IROperand match = sc_temp(sc);
  if (pred == 6) {
    IROperand one = ir_operand_int(1);
    sc_assign(sc, &match, &one);
    ir_operand_destroy(&one);
    static const long long RANGES[][2] = {
        {'a', 'z'}, {'A', 'Z'}, {'0', '9'}, {'_', '_'}};
    for (size_t r = 0; r < 4; r++) {
      IROperand low = ir_operand_int(RANGES[r][0]);
      IROperand high = ir_operand_int(RANGES[r][1]);
      IROperand above = sc_op(sc, ">=", &a, &low, 0);
      IROperand below = sc_op(sc, "<=", &a, &high, 0);
      IROperand within = sc_op(sc, "&", &above, &below, 0);
      IROperand none = ir_operand_int(0);
      sc_if(sc, &within, &match, &none);
      ir_operand_destroy(&low);
      ir_operand_destroy(&high);
      ir_operand_destroy(&above);
      ir_operand_destroy(&below);
      ir_operand_destroy(&within);
      ir_operand_destroy(&none);
    }
  } else if (rhs_kind == 2) {
    IROperand b_at = sc_address(sc, &insn->arguments[3], &loop.index, size);
    IROperand b = sc_load(sc, &b_at, size, 0, size == 1);
    sc_binary(sc, &match, PREDICATES[pred], &a, &b, 0, 0);
    ir_operand_destroy(&b_at);
    ir_operand_destroy(&b);
  } else {
    sc_binary(sc, &match, PREDICATES[pred], &a, &insn->arguments[3], 0, 0);
  }
  char *go_on = sc_label(sc);
  sc_branch_zero(sc, &match, go_on);
  sc_assign(sc, &hit, &loop.index);
  sc_jump(sc, found);
  sc_label_here(sc, go_on);
  ir_operand_destroy(&at);
  ir_operand_destroy(&a);
  ir_operand_destroy(&match);
  free(go_on);
  sc_loop_end(sc, &loop, 1);
  sc_label_here(sc, found);
  sc_assign(sc, &insn->dest, &hit);
  ir_operand_destroy(&first);
  ir_operand_destroy(&hit);
  ir_operand_destroy(&beyond);
  free(found);
}

static void sc_vloop(Sc *sc, const IRInstruction *insn) {
  long long reduce_op = insn->arguments[0].int_value;
  long long n_arrays = insn->arguments[1].int_value;
  long long n_nodes = insn->arguments[2].int_value;
  long long root = insn->arguments[3].int_value;
  long long n_consts = insn->arguments[4].int_value;
  long long n_scalars = insn->arguments[5].int_value;
  int is_int = insn->op == IR_OP_SIMD_VLOOP_I32;
  int is_f32 = !is_int && insn->float_bits == 32;
  int elem8 = is_int && insn->float_bits == 8;
  int bits = is_int ? 0 : (is_f32 ? 32 : 64);
  long long size = elem8 ? 1 : (is_int || is_f32 ? 4 : 8);
  size_t arrays_at = 7;
  size_t scalars_at = arrays_at + (size_t)n_arrays;
  size_t nodes_at = scalars_at + (size_t)n_scalars;
  size_t consts_at = nodes_at + (size_t)n_nodes * 3;
  IROperand *values = calloc((size_t)n_nodes + 1, sizeof(IROperand));
  if (!values) {
    sc->failed = 1;
    return;
  }
  ScLoop loop = sc_loop(sc, &insn->lhs);
  for (long long node = 0; node < n_nodes; node++) {
    long long tag = insn->arguments[nodes_at + (size_t)node * 3].int_value;
    long long op0 = insn->arguments[nodes_at + (size_t)node * 3 + 1].int_value;
    long long op1 = insn->arguments[nodes_at + (size_t)node * 3 + 2].int_value;
    IROperand out;
    switch (tag) {
    case 0: {
      IROperand at =
          sc_address(sc, &insn->arguments[arrays_at + (size_t)op0], &loop.index, size);
      out = sc_load(sc, &at, size, bits, elem8 && insn->is_unsigned);
      ir_operand_destroy(&at);
      break;
    }
    case 1:
      out = is_int ? ir_operand_copy(&loop.index)
                   : sc_cast(sc, is_f32 ? "float32" : "float64", &loop.index);
      break;
    case 2:
      out = ir_operand_copy(&insn->arguments[consts_at + (size_t)op0]);
      break;
    case 7:
      out = ir_operand_copy(&insn->arguments[scalars_at + (size_t)op0]);
      break;
    default: {
      static const char *const INT_OPS[] = {"+", "-", "*", "/", NULL, "&", "|", "^", "<<", ">>", ">>"};
      const char *text = INT_OPS[tag - 3];
      if (tag >= 11) {
        IROperand raw = sc_op_int(sc, text, &values[op0], op1 & 31);
        out = is_int ? sc_cast(sc, "int32", &raw) : raw;
        if (is_int) {
          ir_operand_destroy(&raw);
        }
      } else {
        IROperand raw = sc_op(sc, text, &values[op0], &values[op1], bits);
        if (is_int) {
          out = sc_cast(sc, "int32", &raw);
          ir_operand_destroy(&raw);
        } else {
          out = raw;
        }
      }
      break;
    }
    }
    values[node] = out;
  }
  if (reduce_op == 1) {
    if (is_int) {
      IROperand sum = sc_op(sc, "+", &insn->dest, &values[root], 0);
      IROperand wrapped = sc_cast(sc, "int32", &sum);
      sc_assign(sc, &insn->dest, &wrapped);
      ir_operand_destroy(&sum);
      ir_operand_destroy(&wrapped);
    } else {
      sc_binary(sc, &insn->dest, "+", &insn->dest, &values[root], bits, 0);
    }
  } else if (reduce_op == 2 || reduce_op == 3) {
    IROperand better = sc_temp(sc);
    sc_binary(sc, &better, reduce_op == 2 ? ">" : "<", &values[root],
              &insn->dest, bits, 0);
    sc_if(sc, &better, &insn->dest, &values[root]);
    ir_operand_destroy(&better);
  } else {
    IROperand at = sc_address(sc, &insn->dest, &loop.index, size);
    sc_store(sc, &at, &values[root], size, bits);
    ir_operand_destroy(&at);
  }
  for (long long node = 0; node < n_nodes; node++) {
    ir_operand_destroy(&values[node]);
  }
  free(values);
  sc_loop_end(sc, &loop, 1);
}

static IROperand sc_outer_uniform(Sc *sc, const IRInstruction *insn, size_t prog,
                                  size_t fconst_at, const IROperand *index) {
  long long micro = insn->arguments[prog].int_value;
  IROperand current = ir_operand_copy(index);
  size_t at = prog + 1;
  static const char *const INT_OPS[] = {NULL, "&", "|", "^", "+", "-", "*", "<<", ">>"};
  static const char *const FLOAT_OPS[] = {"+", "-", "*", "/"};
  for (long long m = 0; m < micro; m++, at += 2) {
    long long op = insn->arguments[at].int_value;
    long long imm = insn->arguments[at + 1].int_value;
    IROperand next;
    if (op >= 1 && op <= 8) {
      next = sc_op_int(sc, INT_OPS[op], &current, imm);
    } else if (op == 9) {
      next = sc_cast(sc, "float64", &current);
    } else if (op >= 10 && op <= 13) {
      next = sc_op(sc, FLOAT_OPS[op - 10], &current,
                   &insn->arguments[fconst_at + (size_t)imm], 64);
    } else {
      next = ir_operand_copy(&current);
    }
    ir_operand_destroy(&current);
    current = next;
  }
  return current;
}

static void sc_outer_lane(Sc *sc, const IRInstruction *insn) {
  static const char *const CHAIN_OPS[] = {"+", "-", "*", "/"};
  long long inner_cmp = insn->arguments[0].int_value;
  long long istep = insn->arguments[1].int_value;
  long long n_chain = insn->arguments[2].int_value;
  long long n_unif = insn->arguments[3].int_value;
  long long i0 = insn->arguments[5].int_value;
  long long seed_mode = insn->arguments[6].int_value;
  size_t chain_at = 8;
  size_t at = chain_at + (size_t)(4 * n_chain);
  size_t unif_at = at;
  for (long long u = 0; u < n_unif; u++) {
    at += 1 + (size_t)(2 * insn->arguments[at].int_value);
  }
  size_t seed_at = at;
  if (seed_mode == 1) {
    at += 1 + (size_t)(2 * insn->arguments[at].int_value);
  }
  size_t fconst_at = at;
  ScLoop outer = sc_loop(sc, &insn->lhs);
  IROperand acc = sc_temp(sc);
  if (seed_mode == 1) {
    IROperand seeded = sc_outer_uniform(sc, insn, seed_at, fconst_at, &outer.index);
    sc_assign(sc, &acc, &seeded);
    ir_operand_destroy(&seeded);
  } else {
    sc_assign(sc, &acc, &insn->arguments[7]);
  }
  IROperand start = ir_operand_int(i0);
  ScLoop inner = sc_loop_from(sc, &start, &insn->rhs, inner_cmp ? "<=" : "<");
  ir_operand_destroy(&start);
  size_t step = chain_at;
  for (long long s = 0; s < n_chain; s++, step += 4) {
    long long op = insn->arguments[step].int_value;
    long long side = insn->arguments[step + 1].int_value;
    long long kind = insn->arguments[step + 2].int_value;
    long long idx = insn->arguments[step + 3].int_value;
    IROperand term;
    if (kind == 0) {
      term = ir_operand_copy(&insn->arguments[fconst_at + (size_t)idx]);
    } else {
      size_t prog = unif_at;
      for (long long skip = idx; skip > 0; skip--) {
        prog += 1 + (size_t)(2 * insn->arguments[prog].int_value);
      }
      term = sc_outer_uniform(sc, insn, prog, fconst_at, &inner.index);
    }
    IROperand next = sc_op(sc, CHAIN_OPS[op], side ? &term : &acc,
                           side ? &acc : &term, 64);
    sc_assign(sc, &acc, &next);
    ir_operand_destroy(&term);
    ir_operand_destroy(&next);
  }
  sc_loop_end(sc, &inner, istep);
  sc_binary(sc, &insn->dest, "+", &insn->dest, &acc, 64, 0);
  ir_operand_destroy(&acc);
  sc_loop_end(sc, &outer, 1);
}

int ir_scalarize_opcode_is_kernel(IROpcode op) {
  switch (op) {
  case IR_OP_SELECT:
  case IR_OP_ROTATE_ADD:
  case IR_OP_COUNT_WORD_STARTS:
  case IR_OP_MEMCPY_INLINE:
  case IR_OP_SIMD_SUM_I32:
  case IR_OP_SIMD_SUM_U8:
  case IR_OP_SIMD_BYTE_MAP:
  case IR_OP_SIMD_FILL:
  case IR_OP_SIMD_COPY:
  case IR_OP_SIMD_MATMUL_N32:
  case IR_OP_SIMD_INSERTION_SORT_I32:
  case IR_OP_SIMD_DOT_I32:
  case IR_OP_SIMD_DOT_I8:
  case IR_OP_SIMD_SLP_MAC_I32:
  case IR_OP_SIMD_SLP_MAC_I8:
  case IR_OP_SIMD_SCALE_I32:
  case IR_OP_SIMD_CLAMP_I32:
  case IR_OP_SIMD_REVERSE_COPY_I32:
  case IR_OP_LOWER_BOUND_I32:
  case IR_OP_PREFIX_SUM_I32:
  case IR_OP_SIMD_MINMAX_I32:
  case IR_OP_SIMD_SUM_F64:
  case IR_OP_SIMD_SUM_F32:
  case IR_OP_SIMD_DOT_F64:
  case IR_OP_SIMD_DOT_F32:
  case IR_OP_SIMD_AFFINE_MAP_F64:
  case IR_OP_SIMD_AFFINE_MAP_F32:
  case IR_OP_SIMD_EXP_F32:
  case IR_OP_SIMD_SILU_F32:
  case IR_OP_SIMD_I2F_REDUCE_F64:
  case IR_OP_SIMD_VLOOP_F64:
  case IR_OP_SIMD_VLOOP_I32:
  case IR_OP_SIMD_FIND:
  case IR_OP_SIMD_OUTER_LANE_F64:
  case IR_OP_SIMD_LCG_U32:
  case IR_OP_SIMD_LANES:
  case IR_OP_PREFETCH:
  case IR_OP_SAFETY_CHECK:
  case IR_OP_PHI:
    return 1;
  default:
    return 0;
  }
}

static int sc_expand(Sc *sc, const IRInstruction *insn, char *why,
                     size_t why_size) {
  switch (insn->op) {
  case IR_OP_SELECT:
    sc_select(sc, insn);
    return 1;
  case IR_OP_ROTATE_ADD:
    sc_rotate_add(sc, insn);
    return 1;
  case IR_OP_COUNT_WORD_STARTS:
    sc_count_word_starts(sc, insn);
    return 1;
  case IR_OP_MEMCPY_INLINE:
    sc_memcpy_inline(sc, insn);
    return 1;
  case IR_OP_SIMD_SUM_I32:
  case IR_OP_SIMD_SUM_U8:
    sc_int_sum(sc, insn);
    return 1;
  case IR_OP_SIMD_BYTE_MAP:
    sc_byte_map(sc, insn);
    return 1;
  case IR_OP_SIMD_FILL:
    if (!sc_fill(sc, insn)) {
      snprintf(why, why_size, "simd_fill has a mode it does not spell");
      return 0;
    }
    return 1;
  case IR_OP_SIMD_COPY:
    sc_copy(sc, insn);
    return 1;
  case IR_OP_SIMD_INSERTION_SORT_I32:
    sc_insertion_sort(sc, insn);
    return 1;
  case IR_OP_SIMD_DOT_I32:
  case IR_OP_SIMD_DOT_I8:
    sc_int_dot(sc, insn);
    return 1;
  case IR_OP_SIMD_SLP_MAC_I32:
  case IR_OP_SIMD_SLP_MAC_I8:
    sc_slp_mac(sc, insn);
    return 1;
  case IR_OP_SIMD_SCALE_I32:
  case IR_OP_SIMD_CLAMP_I32:
  case IR_OP_SIMD_REVERSE_COPY_I32:
    sc_int_map(sc, insn);
    return 1;
  case IR_OP_LOWER_BOUND_I32:
    sc_lower_bound(sc, insn);
    return 1;
  case IR_OP_PREFIX_SUM_I32:
    sc_prefix_sum(sc, insn);
    return 1;
  case IR_OP_SIMD_MINMAX_I32:
    sc_minmax(sc, insn);
    return 1;
  case IR_OP_SIMD_SUM_F64:
  case IR_OP_SIMD_SUM_F32:
    sc_float_sum(sc, insn);
    return 1;
  case IR_OP_SIMD_DOT_F64:
  case IR_OP_SIMD_DOT_F32:
    sc_float_dot(sc, insn);
    return 1;
  case IR_OP_SIMD_AFFINE_MAP_F64:
  case IR_OP_SIMD_AFFINE_MAP_F32:
    sc_float_affine(sc, insn);
    return 1;
  case IR_OP_SIMD_EXP_F32:
  case IR_OP_SIMD_SILU_F32:
    sc_float_map(sc, insn);
    return 1;
  case IR_OP_SIMD_I2F_REDUCE_F64:
    sc_i2f_reduce(sc, insn);
    return 1;
  case IR_OP_SIMD_VLOOP_F64:
  case IR_OP_SIMD_VLOOP_I32:
    sc_vloop(sc, insn);
    return 1;
  case IR_OP_SIMD_FIND:
    sc_find(sc, insn);
    return 1;
  case IR_OP_SIMD_OUTER_LANE_F64:
    sc_outer_lane(sc, insn);
    return 1;
  case IR_OP_SIMD_LCG_U32:
    sc_lcg(sc, insn);
    return 1;
  case IR_OP_SIMD_LANES:
    sc_lanes(sc, insn);
    return 1;
  case IR_OP_PREFETCH:
  case IR_OP_SAFETY_CHECK:
    return 1;
  default:
    snprintf(why, why_size, "'%s' at line %zu has no scalar spelling",
             ir_opcode_name(insn->op), (size_t)insn->location.line);
    return 0;
  }
}

IRFunction *ir_scalarize_function(IRFunction *function, char *why,
                                  size_t why_size) {
  int needed = 0;
  for (size_t i = 0; i < function->instruction_count && !needed; i++) {
    needed = ir_scalarize_opcode_is_kernel(function->instructions[i].op);
  }
  if (!needed) {
    return function;
  }
  IRFunction *view = ir_function_create(function->name);
  if (!view ||
      !ir_function_set_parameters(view, (const char **)function->parameter_names,
                                  (const char **)function->parameter_types,
                                  function->parameter_count)) {
    return NULL;
  }
  view->return_type_name =
      function->return_type_name ? mettle_strdup(function->return_type_name) : NULL;
  view->location = function->location;
  view->is_pure = function->is_pure;
  int fresh = 0;
  Sc sc;
  sc.out = view;
  sc.fresh = &fresh;
  sc.failed = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *insn = &function->instructions[i];
    sc.at = insn;
    if (ir_scalarize_opcode_is_kernel(insn->op)) {
      if (!sc_expand(&sc, insn, why, why_size)) {
        ir_function_destroy(view);
        return NULL;
      }
      continue;
    }
    IRInstruction cloned = {0};
    if (!ir_clone_instruction_plain(insn, &cloned) ||
        !ir_function_append_instruction(view, &cloned)) {
      sc.failed = 1;
    }
    ir_instruction_destroy_storage(&cloned);
  }
  if (sc.failed) {
    snprintf(why, why_size, "ran out of memory spelling its kernels");
    ir_function_destroy(view);
    return NULL;
  }
  return view;
}
