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
  if (!reach || !queue) {
    free(reach);
    free(queue);
    e->error = 1;
    return 0;
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
    } else if (!encode_function(e, fn, &fns)) {
      break;
    }
  }

  free(reach);
  free(queue);
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
