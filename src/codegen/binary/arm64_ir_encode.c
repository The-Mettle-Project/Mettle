#include "codegen/binary/arm64_ir.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Scratch registers: lhs, rhs, result, and an aux for msub (modulo). */
#define R_LHS ARM64_X9
#define R_RHS ARM64_X10
#define R_RES ARM64_X11
#define R_AUX ARM64_X12

/* Stack-slot map: every distinct temp/local/param name gets one 8-byte slot. */
typedef struct {
  const char **names;
  int count;
  int cap;
} SlotMap;

static int slot_of(SlotMap *s, const char *name) {
  for (int i = 0; i < s->count; i++) {
    if (s->names[i] == name || strcmp(s->names[i], name) == 0) {
      return i;
    }
  }
  if (s->count == s->cap) {
    int cap = s->cap ? s->cap * 2 : 32;
    const char **n = realloc(s->names, (size_t)cap * sizeof(*n));
    if (!n) {
      return -1;
    }
    s->names = n;
    s->cap = cap;
  }
  s->names[s->count] = name;
  return s->count++;
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

/* Load an IR value operand (temp/local/int) into `scratch` (or a chosen reg). */
static Arm64Reg load_into(Arm64Emit *e, SlotMap *s, const IROperand *op,
                          Arm64Reg dest) {
  switch (op->kind) {
  case IR_OPERAND_INT:
    emit_imm(e, dest, (uint64_t)op->int_value);
    return dest;
  case IR_OPERAND_TEMP:
  case IR_OPERAND_SYMBOL: {
    int slot = slot_of(s, op->name);
    if (slot < 0) {
      e->error = 1;
      return dest;
    }
    arm64_emit_word(e, arm64_ldr_imm(1, dest, ARM64_SP, 8 * slot));
    return dest;
  }
  default:
    e->error = 1;
    return dest;
  }
}

static void store_dest(Arm64Emit *e, SlotMap *s, const IROperand *dst,
                       Arm64Reg src) {
  int slot = slot_of(s, dst->name);
  if (slot < 0) {
    e->error = 1;
    return;
  }
  arm64_emit_word(e, arm64_str_imm(1, src, ARM64_SP, 8 * slot));
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
  Arm64Reg a = load_into(e, s, &in->lhs, R_LHS);
  Arm64Reg b = load_into(e, s, &in->rhs, R_RHS);
  const char *op = in->text;
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

/* Lower one function body. `fns` (if non-NULL) maps callee names to entry
 * labels so IR_OP_CALL can resolve a cross-function bl. */
static int encode_function(Arm64Emit *e, const IRFunction *fn, LblMap *fns) {
  SlotMap slots = {0};
  LblMap labels = {0};

  for (size_t i = 0; i < fn->parameter_count; i++) {
    slot_of(&slots, fn->parameter_names[i]);
  }
  for (size_t i = 0; i < fn->instruction_count; i++) {
    const IRInstruction *in = &fn->instructions[i];
    const IROperand *ops[3] = {&in->dest, &in->lhs, &in->rhs};
    for (int k = 0; k < 3; k++) {
      if (ops[k]->kind == IR_OPERAND_TEMP || ops[k]->kind == IR_OPERAND_SYMBOL) {
        slot_of(&slots, ops[k]->name);
      }
    }
    for (size_t k = 0; k < in->argument_count; k++) {
      const IROperand *a = &in->arguments[k];
      if (a->kind == IR_OPERAND_TEMP || a->kind == IR_OPERAND_SYMBOL) {
        slot_of(&slots, a->name);
      }
    }
  }

  int frame = (slots.count * 8 + 15) & ~15;
  if (frame > 4080) {
    e->error = 1;
  }
  if (e->error || !arm64_emit_prologue(e, frame, NULL, 0)) {
    goto done;
  }
  for (size_t i = 0; i < fn->parameter_count && i < 8; i++) {
    arm64_emit_word(e, arm64_str_imm(1, (Arm64Reg)(ARM64_X0 + i), ARM64_SP,
                                     8 * (int)i));
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
    case IR_OP_CAST:
      store_dest(e, &slots, &in->dest, load_into(e, &slots, &in->lhs, R_LHS));
      break;
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
      /* Marshal args into x0.. (each loaded straight into its ABI register;
       * all values live on the stack, so nothing is clobbered across the bl). */
      size_t na = in->argument_count > 8 ? 8 : in->argument_count;
      for (size_t k = 0; k < na; k++) {
        load_into(e, &slots, &in->arguments[k], (Arm64Reg)(ARM64_X0 + k));
      }
      arm64_emit_bl(e, label_for(e, fns, in->text));
      if (in->dest.kind == IR_OPERAND_TEMP ||
          in->dest.kind == IR_OPERAND_SYMBOL) {
        store_dest(e, &slots, &in->dest, ARM64_X0);
      }
      break;
    }
    case IR_OP_RETURN:
      if (in->lhs.kind != IR_OPERAND_NONE) {
        arm64_emit_mov(e, 1, ARM64_X0, load_into(e, &slots, &in->lhs, R_LHS));
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
  free(labels.names);
  free(labels.ids);
  return e->error ? 0 : 1;
}

int arm64_ir_encode_function(Arm64Emit *e, const IRFunction *fn) {
  return encode_function(e, fn, NULL);
}

int arm64_ir_encode_program(Arm64Emit *e, const IRProgram *prog,
                            const char *entry) {
  LblMap fns = {0};
  if (!entry) {
    entry = "main";
  }
  /* Reserve an entry label for every function so any call resolves. */
  for (size_t i = 0; i < prog->function_count; i++) {
    label_for(e, &fns, prog->functions[i]->name);
  }

  /* _start: call the entry function, then exit(x0). */
  arm64_emit_bl(e, label_for(e, &fns, entry));
  arm64_emit_word(e, arm64_movz(1, ARM64_X8, 93, 0)); /* exit syscall */
  arm64_emit_word(e, 0xD4000001u);                    /* svc #0 */

  for (size_t i = 0; i < prog->function_count && !e->error; i++) {
    const IRFunction *fn = prog->functions[i];
    arm64_bind_label(e, label_for(e, &fns, fn->name));
    if (!encode_function(e, fn, &fns)) {
      break;
    }
  }

  free(fns.names);
  free(fns.ids);
  return e->error ? 0 : 1;
}

/* ---- minimal static AArch64 ELF executable ------------------------------ */

#define ELF_BASE 0x400000u
#define ELF_HDRS 120u

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
