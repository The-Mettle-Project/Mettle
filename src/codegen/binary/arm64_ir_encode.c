#include "codegen/binary/arm64_ir.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Load address and header size of the single PT_LOAD segment (see
 * arm64_write_elf). An embedded string's virtual address is ELF_BASE +
 * ELF_HDRS + its byte offset in the code blob. */
#define ELF_BASE 0x400000u
#define ELF_HDRS 120u

/* Scratch registers: lhs, rhs, result, and an aux for msub (modulo). */
#define R_LHS ARM64_X9
#define R_RHS ARM64_X10
#define R_RES ARM64_X11
#define R_AUX ARM64_X12

/* Stack-slot map: each distinct name gets a byte offset into the frame. Scalars
 * and pointers take 8 bytes; an array local takes count*elem_size (8-aligned)
 * so &array + i*elem_size addresses its elements. */
typedef struct {
  const char **names;
  int *offs;
  int count;
  int cap;
  int frame; /* running total bytes */
} SlotMap;

/* Byte size of an array element by its type name. */
static int type_elem_size(const char *t) {
  if (!t) return 8;
  if (strchr(t, '*')) return 8; /* pointer */
  if (strstr(t, "64")) return 8;
  if (strstr(t, "32")) return 4;
  if (strstr(t, "16")) return 2;
  if (strstr(t, "8")) return 1;
  if (strcmp(t, "bool") == 0) return 1;
  return 8;
}

/* Frame bytes a DECLARE_LOCAL needs from its type text (e.g. "int64[4]"). */
static int local_size_bytes(const char *text) {
  if (!text) return 8;
  const char *lb = strchr(text, '[');
  if (lb) {
    int count = atoi(lb + 1);
    char buf[64];
    size_t n = (size_t)(lb - text);
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    memcpy(buf, text, n);
    buf[n] = 0;
    int total = count * type_elem_size(buf);
    return total > 0 ? total : 8;
  }
  return 8;
}

/* Find or allocate `name`'s slot (size rounded up to 8); returns its offset. */
static int slot_alloc(SlotMap *s, const char *name, int size_bytes) {
  for (int i = 0; i < s->count; i++) {
    if (s->names[i] == name || strcmp(s->names[i], name) == 0) {
      return s->offs[i];
    }
  }
  if (s->count == s->cap) {
    int cap = s->cap ? s->cap * 2 : 32;
    const char **n = realloc(s->names, (size_t)cap * sizeof(*n));
    int *o = realloc(s->offs, (size_t)cap * sizeof(*o));
    if (n) s->names = n;
    if (o) s->offs = o;
    if (!n || !o) {
      return -1;
    }
    s->cap = cap;
  }
  int sz = size_bytes <= 0 ? 8 : ((size_bytes + 7) & ~7);
  int off = s->frame;
  s->frame += sz;
  s->names[s->count] = name;
  s->offs[s->count] = off;
  s->count++;
  return off;
}

/* Byte offset of a name's slot (default 8-byte scalar if not seen yet). */
static int slot_off(SlotMap *s, const char *name) {
  return slot_alloc(s, name, 8);
}

/* IEEE-754 bit pattern of a FLOAT operand at its declared width. */
static uint64_t ieee_bits(const IROperand *op) {
  if (op->float_bits == 32) {
    float f = (float)op->float_value;
    uint32_t b;
    memcpy(&b, &f, 4);
    return b;
  }
  double d = op->float_value;
  uint64_t b;
  memcpy(&b, &d, 8);
  return b;
}

/* A name -> emit-label-id map, shared use for both branch labels (per function)
 * and function entry labels (whole program). */
typedef struct {
  const char **names;
  int *ids;
  int count;
  int cap;
} LblMap;

static int label_for(Arm64Emit *e, LblMap *m, const char *name) {
  for (int i = 0; i < m->count; i++) {
    if (m->names[i] == name || strcmp(m->names[i], name) == 0) {
      return m->ids[i];
    }
  }
  if (m->count == m->cap) {
    int cap = m->cap ? m->cap * 2 : 32;
    const char **n = realloc(m->names, (size_t)cap * sizeof(*n));
    int *ids = realloc(m->ids, (size_t)cap * sizeof(*ids));
    if (n) m->names = n;
    if (ids) m->ids = ids;
    if (!n || !ids) {
      e->error = 1;
      return 0;
    }
    m->cap = cap;
  }
  m->names[m->count] = name;
  m->ids[m->count] = arm64_new_label(e);
  return m->ids[m->count++];
}

static void emit_imm(Arm64Emit *e, Arm64Reg rd, uint64_t v) {
  arm64_emit_word(e, arm64_movz(1, rd, (uint16_t)(v & 0xFFFF), 0));
  if ((v >> 16) & 0xFFFF)
    arm64_emit_word(e, arm64_movk(1, rd, (uint16_t)((v >> 16) & 0xFFFF), 1));
  if ((v >> 32) & 0xFFFF)
    arm64_emit_word(e, arm64_movk(1, rd, (uint16_t)((v >> 32) & 0xFFFF), 2));
  if ((v >> 48) & 0xFFFF)
    arm64_emit_word(e, arm64_movk(1, rd, (uint16_t)((v >> 48) & 0xFFFF), 3));
}

/* A set of value names known to hold a floating-point value. Needed because the
 * IR does not reliably tag a float on every operand use, yet the AAPCS64 ABI
 * passes/returns floats in v-registers -- so calls, returns, and arguments must
 * know float-ness. */
typedef struct {
  const char **names;
  int count;
  int cap;
} StrSet;

static int set_has(const StrSet *s, const char *n) {
  if (!n) return 0;
  for (int i = 0; i < s->count; i++) {
    if (s->names[i] == n || strcmp(s->names[i], n) == 0) return 1;
  }
  return 0;
}
static void set_add(StrSet *s, const char *n) {
  if (!n || set_has(s, n)) return;
  if (s->count == s->cap) {
    int cap = s->cap ? s->cap * 2 : 32;
    const char **p = realloc(s->names, (size_t)cap * sizeof(*p));
    if (!p) return;
    s->names = p;
    s->cap = cap;
  }
  s->names[s->count++] = n;
}

static int operand_is_float(const StrSet *fs, const IROperand *op) {
  if (op->kind == IR_OPERAND_FLOAT || op->float_bits != 0) return 1;
  if ((op->kind == IR_OPERAND_TEMP || op->kind == IR_OPERAND_SYMBOL))
    return set_has(fs, op->name);
  return 0;
}

static int prog_fn_index(const IRProgram *prog, const char *name) {
  if (!name) return -1;
  for (size_t i = 0; i < prog->function_count; i++) {
    if (strcmp(prog->functions[i]->name, name) == 0) return (int)i;
  }
  return -1;
}

/* Populate `fs` with every value name that holds a float in `fn` (params,
 * float locals, float-producing instructions). `retf` (callee returns-float
 * flags) lets a call result be recognized as float. */
static void build_float_set(const IRFunction *fn, const IRProgram *prog,
                            const int *retf, StrSet *fs) {
  for (size_t i = 0; i < fn->parameter_count; i++) {
    if (fn->parameter_types && fn->parameter_types[i] &&
        strstr(fn->parameter_types[i], "float")) {
      set_add(fs, fn->parameter_names[i]);
    }
  }
  for (size_t i = 0; i < fn->instruction_count; i++) {
    const IRInstruction *in = &fn->instructions[i];
    if (in->op == IR_OP_DECLARE_LOCAL && in->dest.name && in->text &&
        strstr(in->text, "float") && !strchr(in->text, '[')) {
      set_add(fs, in->dest.name);
    }
  }
  for (int pass = 0; pass < 4; pass++) {
    for (size_t i = 0; i < fn->instruction_count; i++) {
      const IRInstruction *in = &fn->instructions[i];
      if (in->dest.kind != IR_OPERAND_TEMP && in->dest.kind != IR_OPERAND_SYMBOL)
        continue;
      switch (in->op) {
      case IR_OP_BINARY:
      case IR_OP_UNARY:
      case IR_OP_LOAD:
        if (in->is_float) set_add(fs, in->dest.name);
        break;
      case IR_OP_CAST:
        if (in->dest.float_bits != 0) set_add(fs, in->dest.name);
        break;
      case IR_OP_ASSIGN:
        if (operand_is_float(fs, &in->lhs)) set_add(fs, in->dest.name);
        break;
      case IR_OP_CALL: {
        int ci = retf ? prog_fn_index(prog, in->text) : -1;
        if (ci >= 0 && retf[ci]) set_add(fs, in->dest.name);
        break;
      }
      default:
        break;
      }
    }
  }
}

static int fn_returns_float(const IRFunction *fn, const IRProgram *prog,
                            const int *retf) {
  StrSet fs = {0};
  build_float_set(fn, prog, retf, &fs);
  int rf = 0;
  for (size_t i = 0; i < fn->instruction_count; i++) {
    const IRInstruction *in = &fn->instructions[i];
    if (in->op == IR_OP_RETURN && in->lhs.kind != IR_OPERAND_NONE &&
        operand_is_float(&fs, &in->lhs)) {
      rf = 1;
      break;
    }
  }
  free(fs.names);
  return rf;
}

/* Load an IR value operand (temp/local/int/float) into `dest` as raw 64-bit
 * bits (a float operand yields its IEEE pattern). */
static Arm64Reg load_into(Arm64Emit *e, SlotMap *s, const IROperand *op,
                          Arm64Reg dest) {
  switch (op->kind) {
  case IR_OPERAND_INT:
    emit_imm(e, dest, (uint64_t)op->int_value);
    return dest;
  case IR_OPERAND_FLOAT:
    emit_imm(e, dest, ieee_bits(op));
    return dest;
  case IR_OPERAND_TEMP:
  case IR_OPERAND_SYMBOL: {
    int off = slot_off(s, op->name);
    if (off < 0) {
      e->error = 1;
      return dest;
    }
    arm64_emit_word(e, arm64_ldr_imm(1, dest, ARM64_SP, off));
    return dest;
  }
  default:
    e->error = 1;
    return dest;
  }
}

static void store_dest(Arm64Emit *e, SlotMap *s, const IROperand *dst,
                       Arm64Reg src) {
  int off = slot_off(s, dst->name);
  if (off < 0) {
    e->error = 1;
    return;
  }
  arm64_emit_word(e, arm64_str_imm(1, src, ARM64_SP, off));
}

/* rd = sp + off (address of a local's slot), valid for any frame size. */
static void emit_lea_local(Arm64Emit *e, Arm64Reg rd, int off) {
  if (off <= 4095) {
    arm64_emit_word(e, arm64_add_imm(1, rd, ARM64_SP, (uint32_t)off, 0));
  } else {
    arm64_emit_word(e, arm64_mov_sp(rd, ARM64_SP));
    emit_imm(e, R_AUX, (uint64_t)off);
    arm64_emit_word(e, arm64_add_reg(1, rd, rd, R_AUX));
  }
}

static int cmp_cond(const char *op) {
  if (strcmp(op, "==") == 0) return ARM64_EQ;
  if (strcmp(op, "!=") == 0) return ARM64_NE;
  if (strcmp(op, "<") == 0) return ARM64_LT;
  if (strcmp(op, "<=") == 0) return ARM64_LE;
  if (strcmp(op, ">") == 0) return ARM64_GT;
  if (strcmp(op, ">=") == 0) return ARM64_GE;
  return -1;
}

static void lower_binary(Arm64Emit *e, SlotMap *s, const IRInstruction *in) {
  const char *op = in->text;

  /* Floating-point binary: load operand bits into d0/d1, operate, store the
   * result bits. A comparison yields a 0/1 integer via fcmp + cset. */
  if (in->is_float) {
    int d = in->float_bits != 32;
    arm64_emit_word(e, arm64_fmov_gp(d, 0, load_into(e, s, &in->lhs, R_LHS)));
    arm64_emit_word(e, arm64_fmov_gp(d, 1, load_into(e, s, &in->rhs, R_RHS)));
    int fcc = cmp_cond(op);
    if (fcc >= 0) {
      arm64_emit_word(e, arm64_fcmp(d, 0, 1));
      arm64_emit_word(e, arm64_cset(1, R_RES, (Arm64Cond)fcc));
    } else if (strcmp(op, "+") == 0) {
      arm64_emit_word(e, arm64_fadd(d, 0, 0, 1));
    } else if (strcmp(op, "-") == 0) {
      arm64_emit_word(e, arm64_fsub(d, 0, 0, 1));
    } else if (strcmp(op, "*") == 0) {
      arm64_emit_word(e, arm64_fmul(d, 0, 0, 1));
    } else if (strcmp(op, "/") == 0) {
      arm64_emit_word(e, arm64_fdiv(d, 0, 0, 1));
    } else {
      e->error = 1;
      return;
    }
    if (fcc < 0) {
      arm64_emit_word(e, arm64_fmov_to_gp(d, R_RES, 0));
    }
    store_dest(e, s, &in->dest, R_RES);
    return;
  }

  Arm64Reg a = load_into(e, s, &in->lhs, R_LHS);
  Arm64Reg b = load_into(e, s, &in->rhs, R_RHS);
  int cc = cmp_cond(op);
  if (cc >= 0) {
    arm64_emit_word(e, arm64_cmp_reg(1, a, b));
    arm64_emit_word(e, arm64_cset(1, R_RES, (Arm64Cond)cc));
  } else if (strcmp(op, "+") == 0) {
    arm64_emit_word(e, arm64_add_reg(1, R_RES, a, b));
  } else if (strcmp(op, "-") == 0) {
    arm64_emit_word(e, arm64_sub_reg(1, R_RES, a, b));
  } else if (strcmp(op, "*") == 0) {
    arm64_emit_word(e, arm64_mul(1, R_RES, a, b));
  } else if (strcmp(op, "/") == 0) {
    arm64_emit_word(e, in->is_unsigned ? arm64_udiv(1, R_RES, a, b)
                                       : arm64_sdiv(1, R_RES, a, b));
  } else if (strcmp(op, "%") == 0) {
    arm64_emit_word(e, in->is_unsigned ? arm64_udiv(1, R_AUX, a, b)
                                       : arm64_sdiv(1, R_AUX, a, b));
    arm64_emit_word(e, arm64_msub(1, R_RES, R_AUX, b, a));
  } else if (strcmp(op, "&") == 0) {
    arm64_emit_word(e, arm64_and_reg(1, R_RES, a, b));
  } else if (strcmp(op, "|") == 0) {
    arm64_emit_word(e, arm64_orr_reg(1, R_RES, a, b));
  } else if (strcmp(op, "^") == 0) {
    arm64_emit_word(e, arm64_eor_reg(1, R_RES, a, b));
  } else if (strcmp(op, "<<") == 0) {
    arm64_emit_word(e, arm64_lslv(1, R_RES, a, b));
  } else if (strcmp(op, ">>") == 0) {
    arm64_emit_word(e, in->is_unsigned ? arm64_lsrv(1, R_RES, a, b)
                                       : arm64_asrv(1, R_RES, a, b));
  } else {
    e->error = 1;
    return;
  }
  store_dest(e, s, &in->dest, R_RES);
}

static void lower_unary(Arm64Emit *e, SlotMap *s, const IRInstruction *in) {
  Arm64Reg a = load_into(e, s, &in->lhs, R_LHS);
  const char *op = in->text;
  if (strcmp(op, "-") == 0) {
    arm64_emit_word(e, arm64_neg(1, R_RES, a));
  } else if (strcmp(op, "~") == 0) {
    arm64_emit_word(e, arm64_mvn(1, R_RES, a));
  } else if (strcmp(op, "!") == 0) {
    arm64_emit_word(e, arm64_cmp_imm(1, a, 0, 0));
    arm64_emit_word(e, arm64_cset(1, R_RES, ARM64_EQ));
  } else {
    e->error = 1;
    return;
  }
  store_dest(e, s, &in->dest, R_RES);
}

/* Lower one function body. `fns` maps callee names to entry labels so IR_OP_CALL
 * can resolve a cross-function bl; `prog`/`retf` drive the float ABI (which
 * callees return floats). All may be NULL for the single-function path. */
static int encode_function(Arm64Emit *e, const IRFunction *fn, LblMap *fns,
                           const IRProgram *prog, const int *retf) {
  SlotMap slots = {0};
  LblMap labels = {0};
  StrSet fs = {0};
  build_float_set(fn, prog, retf, &fs);

  /* Slot allocation order: parameters first (so x0../v0.. home to known
   * offsets), then declared locals at their real sizes (arrays!), then any
   * remaining temps/symbols at 8 bytes. */
  for (size_t i = 0; i < fn->parameter_count; i++) {
    slot_alloc(&slots, fn->parameter_names[i], 8);
  }
  for (size_t i = 0; i < fn->instruction_count; i++) {
    const IRInstruction *in = &fn->instructions[i];
    if (in->op == IR_OP_DECLARE_LOCAL && in->dest.kind == IR_OPERAND_SYMBOL &&
        in->dest.name) {
      slot_alloc(&slots, in->dest.name, local_size_bytes(in->text));
    }
  }
  for (size_t i = 0; i < fn->instruction_count; i++) {
    const IRInstruction *in = &fn->instructions[i];
    const IROperand *ops[3] = {&in->dest, &in->lhs, &in->rhs};
    for (int k = 0; k < 3; k++) {
      if (ops[k]->kind == IR_OPERAND_TEMP || ops[k]->kind == IR_OPERAND_SYMBOL) {
        slot_off(&slots, ops[k]->name);
      }
    }
    for (size_t k = 0; k < in->argument_count; k++) {
      const IROperand *a = &in->arguments[k];
      if (a->kind == IR_OPERAND_TEMP || a->kind == IR_OPERAND_SYMBOL) {
        slot_off(&slots, a->name);
      }
    }
  }

  int frame = (slots.frame + 15) & ~15;
  if (e->error || !arm64_emit_prologue(e, frame, NULL, 0)) {
    goto done;
  }
  /* Home incoming parameters. GP and FP arguments are counted independently
   * (AAPCS64): a float param arrives in v<fp>, an integer in x<gp>. */
  {
    int gp = 0, fp = 0;
    for (size_t i = 0; i < fn->parameter_count; i++) {
      const char *ty = fn->parameter_types ? fn->parameter_types[i] : NULL;
      int isf = ty && strstr(ty, "float") != NULL;
      int off = slot_off(&slots, fn->parameter_names[i]);
      if (isf) {
        if (fp < 8) {
          int d = !strstr(ty, "32");
          arm64_emit_word(e, arm64_str_fp(d, fp, ARM64_SP, off));
        }
        fp++;
      } else {
        if (gp < 8) {
          arm64_emit_word(e, arm64_str_imm(1, (Arm64Reg)(ARM64_X0 + gp),
                                           ARM64_SP, off));
        }
        gp++;
      }
    }
  }

  for (size_t i = 0; i < fn->instruction_count && !e->error; i++) {
    const IRInstruction *in = &fn->instructions[i];
    switch (in->op) {
    case IR_OP_NOP:
    case IR_OP_DECLARE_LOCAL:
      break;
    case IR_OP_LABEL:
      arm64_bind_label(e, label_for(e, &labels, in->text));
      break;
    case IR_OP_JUMP:
      arm64_emit_b(e, label_for(e, &labels, in->text));
      break;
    case IR_OP_BRANCH_ZERO:
      arm64_emit_cbz(e, 1, load_into(e, &slots, &in->lhs, R_LHS),
                     label_for(e, &labels, in->text));
      break;
    case IR_OP_BRANCH_EQ: {
      Arm64Reg a = load_into(e, &slots, &in->lhs, R_LHS);
      Arm64Reg b = load_into(e, &slots, &in->rhs, R_RHS);
      arm64_emit_word(e, arm64_cmp_reg(1, a, b));
      arm64_emit_bcond(e, ARM64_EQ, label_for(e, &labels, in->text));
      break;
    }
    case IR_OP_ASSIGN:
      store_dest(e, &slots, &in->dest, load_into(e, &slots, &in->lhs, R_LHS));
      break;
    case IR_OP_CAST: {
      int srcf = in->is_float;
      int dstf = in->dest.float_bits != 0;
      if (srcf && !dstf) { /* float -> int (truncating) */
        int d = in->float_bits != 32;
        arm64_emit_word(e, arm64_fmov_gp(d, 0, load_into(e, &slots, &in->lhs,
                                                         R_LHS)));
        arm64_emit_word(e, arm64_fcvtzs(d, R_RES, 0));
        store_dest(e, &slots, &in->dest, R_RES);
      } else if (!srcf && dstf) { /* int -> float */
        int d = in->dest.float_bits != 32;
        arm64_emit_word(e, arm64_scvtf(d, 0, load_into(e, &slots, &in->lhs,
                                                       R_LHS)));
        arm64_emit_word(e, arm64_fmov_to_gp(d, R_RES, 0));
        store_dest(e, &slots, &in->dest, R_RES);
      } else if (srcf && dstf) { /* float -> float */
        int sd = in->float_bits != 32, dd = in->dest.float_bits != 32;
        arm64_emit_word(e, arm64_fmov_gp(sd, 0, load_into(e, &slots, &in->lhs,
                                                          R_LHS)));
        if (sd != dd) {
          arm64_emit_word(e, arm64_fcvt(dd, 1, 0));
          arm64_emit_word(e, arm64_fmov_to_gp(dd, R_RES, 1));
        } else {
          arm64_emit_word(e, arm64_fmov_to_gp(sd, R_RES, 0));
        }
        store_dest(e, &slots, &in->dest, R_RES);
      } else { /* int -> int: bit copy */
        store_dest(e, &slots, &in->dest, load_into(e, &slots, &in->lhs, R_LHS));
      }
      break;
    }
    case IR_OP_ADDRESS_OF: {
      emit_lea_local(e, R_RES, slot_off(&slots, in->lhs.name));
      store_dest(e, &slots, &in->dest, R_RES);
      break;
    }
    case IR_OP_LOAD: {
      int size = in->rhs.kind == IR_OPERAND_INT ? (int)in->rhs.int_value : 8;
      Arm64Reg addr = load_into(e, &slots, &in->lhs, R_LHS);
      int sx = !in->is_unsigned && !in->is_float;
      switch (size) {
      case 8:
        arm64_emit_word(e, arm64_ldr_imm(1, R_RES, addr, 0));
        break;
      case 4:
        arm64_emit_word(e, arm64_ldr_imm(0, R_RES, addr, 0));
        if (sx) arm64_emit_word(e, arm64_sxtw(R_RES, R_RES));
        break;
      case 2:
        arm64_emit_word(e, arm64_ldrh_imm(R_RES, addr, 0));
        if (sx) arm64_emit_word(e, arm64_sxth(R_RES, R_RES));
        break;
      default:
        arm64_emit_word(e, arm64_ldrb_imm(R_RES, addr, 0));
        if (sx) arm64_emit_word(e, arm64_sxtb(R_RES, R_RES));
        break;
      }
      store_dest(e, &slots, &in->dest, R_RES);
      break;
    }
    case IR_OP_STORE: {
      int size = in->rhs.kind == IR_OPERAND_INT ? (int)in->rhs.int_value : 8;
      Arm64Reg addr = load_into(e, &slots, &in->dest, R_LHS);
      Arm64Reg val = load_into(e, &slots, &in->lhs, R_RHS);
      switch (size) {
      case 8: arm64_emit_word(e, arm64_str_imm(1, val, addr, 0)); break;
      case 4: arm64_emit_word(e, arm64_str_imm(0, val, addr, 0)); break;
      case 2: arm64_emit_word(e, arm64_strh_imm(val, addr, 0)); break;
      default: arm64_emit_word(e, arm64_strb_imm(val, addr, 0)); break;
      }
      break;
    }
    case IR_OP_BINARY:
      lower_binary(e, &slots, in);
      break;
    case IR_OP_UNARY:
      lower_unary(e, &slots, in);
      break;
    case IR_OP_CALL: {
      if (!fns || !in->text) {
        e->error = 1;
        break;
      }
      /* cstr("literal"): embed the bytes in the loaded segment (branched over),
       * and materialize their virtual address into dest -- no actual call. */
      if (strcmp(in->text, "cstr") == 0 && in->argument_count >= 1 &&
          in->arguments[0].kind == IR_OPERAND_STRING &&
          in->arguments[0].name) {
        const char *str = in->arguments[0].name;
        int past = arm64_new_label(e);
        arm64_emit_b(e, past);
        size_t soff = arm64_here(e);
        arm64_emit_bytes(e, str, strlen(str) + 1);
        arm64_bind_label(e, past);
        emit_imm(e, R_RES, (uint64_t)ELF_BASE + ELF_HDRS + soff);
        if (in->dest.kind == IR_OPERAND_TEMP ||
            in->dest.kind == IR_OPERAND_SYMBOL) {
          store_dest(e, &slots, &in->dest, R_RES);
        }
        break;
      }
      /* Marshal args: integers into x0..x7, floats into v0..v7 (counted
       * independently). All values live on the stack, so nothing is clobbered
       * across the bl. */
      {
        int gp = 0, fp = 0;
        for (size_t k = 0; k < in->argument_count; k++) {
          const IROperand *arg = &in->arguments[k];
          int af = operand_is_float(&fs, arg);
          if (af) {
            if (fp < 8) {
              int d = arg->float_bits != 32;
              arm64_emit_word(e, arm64_fmov_gp(d, fp,
                                               load_into(e, &slots, arg,
                                                         R_LHS)));
            }
            fp++;
          } else {
            if (gp < 8) {
              load_into(e, &slots, arg, (Arm64Reg)(ARM64_X0 + gp));
            }
            gp++;
          }
        }
      }
      arm64_emit_bl(e, label_for(e, fns, in->text));
      if (in->dest.kind == IR_OPERAND_TEMP ||
          in->dest.kind == IR_OPERAND_SYMBOL) {
        int ci = (prog && retf) ? prog_fn_index(prog, in->text) : -1;
        int resf = (ci >= 0 && retf[ci]) || set_has(&fs, in->dest.name);
        if (resf) { /* float result arrives in d0/s0 */
          int d = in->dest.float_bits != 32;
          arm64_emit_word(e, arm64_fmov_to_gp(d, R_RES, 0));
          store_dest(e, &slots, &in->dest, R_RES);
        } else {
          store_dest(e, &slots, &in->dest, ARM64_X0);
        }
      }
      break;
    }
    case IR_OP_RETURN:
      if (in->lhs.kind != IR_OPERAND_NONE) {
        if (operand_is_float(&fs, &in->lhs)) { /* float result goes in d0/s0 */
          int d = in->lhs.float_bits != 32;
          arm64_emit_word(e, arm64_fmov_gp(d, 0, load_into(e, &slots, &in->lhs,
                                                           R_LHS)));
        } else {
          arm64_emit_mov(e, 1, ARM64_X0, load_into(e, &slots, &in->lhs, R_LHS));
        }
      }
      arm64_emit_epilogue(e, frame, NULL, 0);
      break;
    default:
      e->error = 1;
      break;
    }
  }

done:
  free(slots.names);
  free(slots.offs);
  free(labels.names);
  free(labels.ids);
  free(fs.names);
  return e->error ? 0 : 1;
}

int arm64_ir_encode_function(Arm64Emit *e, const IRFunction *fn) {
  return encode_function(e, fn, NULL, NULL, NULL);
}

/* I/O intrinsics we provide as hand-written AArch64 stubs (a direct write(2)
 * syscall) instead of compiling the std/io body, which bottoms out in
 * OS-specific externs/strings. `with_newline` distinguishes the println forms;
 * `is_string` distinguishes the cstring printers (print/println) from the int
 * printers (print_int/println_int). cstr is handled inline, not via a stub. */
static int io_stub_intrinsic(const char *name, int *with_newline,
                             int *is_string) {
  if (!name) {
    return 0;
  }
  struct {
    const char *n;
    int nl, str;
  } table[] = {{"println_int", 1, 0}, {"print_int", 0, 0},
               {"println", 1, 1},     {"print", 0, 1}};
  for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
    if (strcmp(name, table[i].n) == 0) {
      if (with_newline) *with_newline = table[i].nl;
      if (is_string) *is_string = table[i].str;
      return 1;
    }
  }
  return 0;
}

/* True for any std/io function the backend handles specially (a printer stub or
 * the inline cstr), so reachability treats it as a leaf. */
static int io_leaf(const char *name) {
  return io_stub_intrinsic(name, NULL, NULL) ||
         (name && strcmp(name, "cstr") == 0);
}

/* Emit a leaf that prints the signed int64 in x0 as decimal (then a newline if
 * with_newline) via the AArch64 Linux write syscall. Builds the digits into a
 * 32-byte stack buffer back-to-front, then write(1, start, len). */
static void emit_int_print(Arm64Emit *e, int with_newline) {
  int l_pos = arm64_new_label(e);
  int l_loop = arm64_new_label(e);
  int l_sign = arm64_new_label(e);
  int l_write = arm64_new_label(e);

  arm64_emit_prologue(e, 32, NULL, 0); /* [sp,#0..31] scratch buffer */
  arm64_emit_word(e, arm64_mov_reg(1, ARM64_X9, ARM64_X0));        /* n */
  arm64_emit_word(e, arm64_add_imm(1, ARM64_X10, ARM64_SP, 31, 0));/* ptr */
  if (with_newline) {
    arm64_emit_word(e, arm64_movz(1, ARM64_X11, 10, 0));           /* '\n' */
    arm64_emit_word(e, arm64_strb_imm(ARM64_X11, ARM64_X10, 0));
    arm64_emit_word(e, arm64_sub_imm(1, ARM64_X10, ARM64_X10, 1, 0));
  }
  arm64_emit_word(e, arm64_movz(1, ARM64_X12, 0, 0));              /* neg=0 */
  arm64_emit_word(e, arm64_cmp_imm(1, ARM64_X9, 0, 0));
  arm64_emit_bcond(e, ARM64_GE, l_pos);
  arm64_emit_word(e, arm64_movz(1, ARM64_X12, 1, 0));              /* neg=1 */
  arm64_emit_word(e, arm64_neg(1, ARM64_X9, ARM64_X9));
  arm64_bind_label(e, l_pos);
  /* n == 0 -> emit '0' and skip the divide loop */
  arm64_emit_cbnz(e, 1, ARM64_X9, l_loop);
  arm64_emit_word(e, arm64_movz(1, ARM64_X11, 48, 0));            /* '0' */
  arm64_emit_word(e, arm64_strb_imm(ARM64_X11, ARM64_X10, 0));
  arm64_emit_word(e, arm64_sub_imm(1, ARM64_X10, ARM64_X10, 1, 0));
  arm64_emit_b(e, l_sign);
  arm64_bind_label(e, l_loop);
  arm64_emit_cbz(e, 1, ARM64_X9, l_sign);
  arm64_emit_word(e, arm64_movz(1, ARM64_X13, 10, 0));
  arm64_emit_word(e, arm64_udiv(1, ARM64_X14, ARM64_X9, ARM64_X13));
  arm64_emit_word(e, arm64_msub(1, ARM64_X15, ARM64_X14, ARM64_X13, ARM64_X9));
  arm64_emit_word(e, arm64_mov_reg(1, ARM64_X9, ARM64_X14));      /* n /= 10 */
  arm64_emit_word(e, arm64_add_imm(1, ARM64_X15, ARM64_X15, 48, 0)); /* +'0' */
  arm64_emit_word(e, arm64_strb_imm(ARM64_X15, ARM64_X10, 0));
  arm64_emit_word(e, arm64_sub_imm(1, ARM64_X10, ARM64_X10, 1, 0));
  arm64_emit_b(e, l_loop);
  arm64_bind_label(e, l_sign);
  arm64_emit_cbz(e, 1, ARM64_X12, l_write);
  arm64_emit_word(e, arm64_movz(1, ARM64_X11, 45, 0));            /* '-' */
  arm64_emit_word(e, arm64_strb_imm(ARM64_X11, ARM64_X10, 0));
  arm64_emit_word(e, arm64_sub_imm(1, ARM64_X10, ARM64_X10, 1, 0));
  arm64_bind_label(e, l_write);
  arm64_emit_word(e, arm64_add_imm(1, ARM64_X1, ARM64_X10, 1, 0));  /* buf */
  arm64_emit_word(e, arm64_add_imm(1, ARM64_X2, ARM64_SP, 32, 0));  /* end */
  arm64_emit_word(e, arm64_sub_reg(1, ARM64_X2, ARM64_X2, ARM64_X1)); /* len */
  arm64_emit_word(e, arm64_movz(1, ARM64_X0, 1, 0));               /* fd=stdout */
  arm64_emit_word(e, arm64_movz(1, ARM64_X8, 64, 0));             /* write */
  arm64_emit_word(e, 0xD4000001u);                                /* svc #0 */
  arm64_emit_epilogue(e, 32, NULL, 0);
}

/* Emit a leaf that writes the NUL-terminated cstring in x0 to stdout (then a
 * newline if with_newline): strlen, then write(1, ptr, len). */
static void emit_str_print(Arm64Emit *e, int with_newline) {
  int l_scan = arm64_new_label(e);
  int l_write = arm64_new_label(e);
  arm64_emit_prologue(e, 16, NULL, 0);
  arm64_emit_word(e, arm64_mov_reg(1, ARM64_X9, ARM64_X0));   /* walker */
  arm64_emit_word(e, arm64_movz(1, ARM64_X10, 0, 0));         /* len */
  arm64_bind_label(e, l_scan);
  arm64_emit_word(e, arm64_ldrb_imm(ARM64_X11, ARM64_X9, 0));
  arm64_emit_cbz(e, 0, ARM64_X11, l_write);                   /* NUL -> done */
  arm64_emit_word(e, arm64_add_imm(1, ARM64_X9, ARM64_X9, 1, 0));
  arm64_emit_word(e, arm64_add_imm(1, ARM64_X10, ARM64_X10, 1, 0));
  arm64_emit_b(e, l_scan);
  arm64_bind_label(e, l_write);
  arm64_emit_word(e, arm64_mov_reg(1, ARM64_X1, ARM64_X0));   /* buf = ptr */
  arm64_emit_word(e, arm64_mov_reg(1, ARM64_X2, ARM64_X10));  /* len */
  arm64_emit_word(e, arm64_movz(1, ARM64_X0, 1, 0));          /* fd=stdout */
  arm64_emit_word(e, arm64_movz(1, ARM64_X8, 64, 0));         /* write */
  arm64_emit_word(e, 0xD4000001u);                            /* svc #0 */
  if (with_newline) {
    arm64_emit_word(e, arm64_movz(1, ARM64_X11, 10, 0));      /* '\n' */
    arm64_emit_word(e, arm64_strb_imm(ARM64_X11, ARM64_SP, 0));
    arm64_emit_word(e, arm64_mov_sp(ARM64_X1, ARM64_SP));     /* buf = sp */
    arm64_emit_word(e, arm64_movz(1, ARM64_X2, 1, 0));        /* len=1 */
    arm64_emit_word(e, arm64_movz(1, ARM64_X0, 1, 0));
    arm64_emit_word(e, arm64_movz(1, ARM64_X8, 64, 0));
    arm64_emit_word(e, 0xD4000001u);
  }
  arm64_emit_epilogue(e, 16, NULL, 0);
}

/* Index of the function named `name`, or -1. */
static int find_fn(const IRProgram *prog, const char *name) {
  for (size_t i = 0; i < prog->function_count; i++) {
    if (strcmp(prog->functions[i]->name, name) == 0) {
      return (int)i;
    }
  }
  return -1;
}

int arm64_ir_encode_program(Arm64Emit *e, const IRProgram *prog,
                            const char *entry) {
  LblMap fns = {0};
  if (!entry) {
    entry = "main";
  }
  size_t n = prog->function_count;
  char *reach = calloc(n ? n : 1, 1);
  int *queue = malloc((n ? n : 1) * sizeof(int));
  int *retf = calloc(n ? n : 1, sizeof(int));
  if (!reach || !queue || !retf) {
    free(reach);
    free(queue);
    free(retf);
    e->error = 1;
    return 0;
  }

  /* Which functions return a float (fixpoint, since a function can return the
   * result of another float-returning call). Drives the v-register ABI. */
  for (size_t iter = 0; iter <= n; iter++) {
    int changed = 0;
    for (size_t i = 0; i < n; i++) {
      if (!retf[i] && fn_returns_float(prog->functions[i], prog, retf)) {
        retf[i] = 1;
        changed = 1;
      }
    }
    if (!changed) break;
  }

  /* Reachability from `entry` over the call graph, treating I/O intrinsics as
   * leaves (their bodies are replaced by stubs, so the std/io internals they
   * would call are not pulled in). Only reachable functions are emitted. */
  int qh = 0, qt = 0;
  int start = find_fn(prog, entry);
  if (start >= 0) {
    queue[qt++] = start;
  }
  while (qh < qt) {
    int fi = queue[qh++];
    if (reach[fi]) {
      continue;
    }
    reach[fi] = 1;
    const IRFunction *f = prog->functions[fi];
    if (io_leaf(f->name)) {
      continue; /* leaf: do not follow into the stdlib body */
    }
    for (size_t k = 0; k < f->instruction_count; k++) {
      const IRInstruction *in = &f->instructions[k];
      if (in->op == IR_OP_CALL && in->text) {
        int ci = find_fn(prog, in->text);
        if (ci >= 0 && !reach[ci]) {
          queue[qt++] = ci;
        }
      }
    }
  }

  /* _start: call the entry function, then exit(x0). */
  arm64_emit_bl(e, label_for(e, &fns, entry));
  arm64_emit_word(e, arm64_movz(1, ARM64_X8, 93, 0)); /* exit syscall */
  arm64_emit_word(e, 0xD4000001u);                    /* svc #0 */

  for (size_t i = 0; i < n && !e->error; i++) {
    if (!reach[i]) {
      continue;
    }
    const IRFunction *fn = prog->functions[i];
    /* cstr is fully inlined at call sites; it is never the target of a bl, so
     * its label is unreferenced and it needs no body. */
    if (strcmp(fn->name, "cstr") == 0) {
      continue;
    }
    arm64_bind_label(e, label_for(e, &fns, fn->name));
    int with_newline = 0, is_string = 0;
    if (io_stub_intrinsic(fn->name, &with_newline, &is_string)) {
      if (is_string) {
        emit_str_print(e, with_newline);
      } else {
        emit_int_print(e, with_newline);
      }
    } else if (!encode_function(e, fn, &fns, prog, retf)) {
      break;
    }
  }

  free(reach);
  free(queue);
  free(retf);
  free(fns.names);
  free(fns.ids);
  return e->error ? 0 : 1;
}

/* ---- minimal static AArch64 ELF executable ------------------------------ */

static void w16(unsigned char *p, uint16_t v) { memcpy(p, &v, 2); }
static void w32(unsigned char *p, uint32_t v) { memcpy(p, &v, 4); }
static void w64(unsigned char *p, uint64_t v) { memcpy(p, &v, 8); }

int arm64_write_elf(const char *path, const unsigned char *code, size_t len) {
  unsigned char h[ELF_HDRS];
  memset(h, 0, sizeof(h));
  uint64_t total = ELF_HDRS + len;
  h[0] = 0x7F; h[1] = 'E'; h[2] = 'L'; h[3] = 'F';
  h[4] = 2; h[5] = 1; h[6] = 1;
  w16(h + 16, 2); w16(h + 18, 183); w32(h + 20, 1);
  w64(h + 24, ELF_BASE + ELF_HDRS); w64(h + 32, 64); w64(h + 40, 0);
  w32(h + 48, 0); w16(h + 52, 64); w16(h + 54, 56); w16(h + 56, 1);
  unsigned char *ph = h + 64;
  w32(ph + 0, 1); w32(ph + 4, 5); w64(ph + 8, 0);
  w64(ph + 16, ELF_BASE); w64(ph + 24, ELF_BASE);
  w64(ph + 32, total); w64(ph + 40, total); w64(ph + 48, 0x1000);

  FILE *f = fopen(path, "wb");
  if (!f) {
    return 0;
  }
  int ok = fwrite(h, 1, ELF_HDRS, f) == ELF_HDRS &&
           fwrite(code, 1, len, f) == len;
  fclose(f);
  return ok;
}
