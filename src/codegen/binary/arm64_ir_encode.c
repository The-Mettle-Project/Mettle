#include "codegen/binary/arm64_ir.h"
#include "codegen/binary_emitter.h"
#include "codegen/binary_emitter_internal.h"

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

typedef struct {
  BinaryEmitter *emitter;
  const IRProgram *program;
  size_t text_section;
  size_t rodata_section;
  unsigned string_id;
} Arm64ObjectContext;

/* Stack-slot map: each distinct name gets a byte offset into the frame. Scalars
 * and pointers take 8 bytes; an array local takes count*elem_size (8-aligned)
 * so &array + i*elem_size addresses its elements. */
typedef struct {
  const char **names;
  int *offs;
  int count;
  int cap;
  int frame; /* running total bytes */
  Arm64ObjectContext *object;
} SlotMap;

static const IRModuleSymbol *module_variable(const SlotMap *slots,
                                             const char *name) {
  if (!slots || !slots->object || !name) return NULL;
  const IRModuleSymbol *symbol =
      ir_program_lookup_symbol(slots->object->program, name);
  return symbol && symbol->kind == IR_MODSYM_VARIABLE ? symbol : NULL;
}

static const char *module_link_name(const IRModuleSymbol *symbol) {
  return symbol && symbol->link_name && symbol->link_name[0]
             ? symbol->link_name
             : (symbol ? symbol->name : NULL);
}

static int object_add_relocation(Arm64Emit *e, Arm64ObjectContext *object,
                                 size_t offset, BinaryRelocationKind kind,
                                 const char *symbol) {
  if (!object || !symbol ||
      !binary_emitter_add_relocation(object->emitter, object->text_section,
                                     offset, kind, symbol, 0)) {
    e->error = 1;
    return 0;
  }
  return 1;
}

/* rd = &symbol using the ELF small position-independent code model. The two
 * zero-immediate instructions are completed by AAELF64 page/lo12 relocations. */
static void emit_symbol_address(Arm64Emit *e, Arm64ObjectContext *object,
                                Arm64Reg rd, const char *symbol) {
  size_t at = arm64_here(e);
  arm64_emit_word(e, 0x90000000u | (uint32_t)rd); /* adrp rd, symbol */
  if (!object_add_relocation(e, object, at,
                             BINARY_RELOCATION_ARM64_ADR_PREL_PG_HI21,
                             symbol)) {
    return;
  }
  at = arm64_here(e);
  arm64_emit_word(e, arm64_add_imm(1, rd, rd, 0, 0));
  object_add_relocation(e, object, at,
                        BINARY_RELOCATION_ARM64_ADD_ABS_LO12_NC, symbol);
}

static int module_type_size(const MtlcType *type) {
  size_t size = mtlc_type_size(type);
  return size == 1 || size == 2 || size == 4 || size == 8 ? (int)size : 8;
}

static int module_type_signed(const MtlcType *type) {
  return type && (type->kind == MTLC_TYPE_INT8 ||
                  type->kind == MTLC_TYPE_INT16 ||
                  type->kind == MTLC_TYPE_INT32 ||
                  type->kind == MTLC_TYPE_INT64);
}

static void emit_load_sized(Arm64Emit *e, Arm64Reg dest, Arm64Reg address,
                            int size, int sign_extend) {
  switch (size) {
  case 8:
    arm64_emit_word(e, arm64_ldr_imm(1, dest, address, 0));
    break;
  case 4:
    arm64_emit_word(e, arm64_ldr_imm(0, dest, address, 0));
    if (sign_extend) arm64_emit_word(e, arm64_sxtw(dest, dest));
    break;
  case 2:
    arm64_emit_word(e, arm64_ldrh_imm(dest, address, 0));
    if (sign_extend) arm64_emit_word(e, arm64_sxth(dest, dest));
    break;
  default:
    arm64_emit_word(e, arm64_ldrb_imm(dest, address, 0));
    if (sign_extend) arm64_emit_word(e, arm64_sxtb(dest, dest));
    break;
  }
}

static void emit_store_sized(Arm64Emit *e, Arm64Reg source,
                             Arm64Reg address, int size) {
  switch (size) {
  case 8: arm64_emit_word(e, arm64_str_imm(1, source, address, 0)); break;
  case 4: arm64_emit_word(e, arm64_str_imm(0, source, address, 0)); break;
  case 2: arm64_emit_word(e, arm64_strh_imm(source, address, 0)); break;
  default: arm64_emit_word(e, arm64_strb_imm(source, address, 0)); break;
  }
}

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

/* Lookup-only variant: byte offset of a name's slot, or -1 if the name has no
 * slot. Parameters and DECLARE_LOCALs are slot-allocated before the body is
 * lowered, so a hit here means the name is function-local and must shadow any
 * module variable of the same name (matching the x86 backend's locals-first
 * resolution order). */
static int slot_find(const SlotMap *s, const char *name) {
  for (int i = 0; i < s->count; i++) {
    if (s->names[i] == name || strcmp(s->names[i], name) == 0) {
      return s->offs[i];
    }
  }
  return -1;
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
  if (!prog || !name) return -1;
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

/* Materialize the address of a NUL-terminated string literal into `dest`.
 * Object emission pools the bytes into .rodata behind a local symbol resolved
 * by page/lo12 relocations; the legacy self-contained path embeds the bytes
 * in the text stream (branched over) at a known virtual address. */
static void emit_string_literal_address(Arm64Emit *e, Arm64ObjectContext *object,
                                        const char *str, Arm64Reg dest) {
  if (!str) str = "";
  if (object) {
    char symbol[64];
    size_t offset = 0;
    snprintf(symbol, sizeof(symbol), ".Lmtlc.str.%u", object->string_id++);
    if (!binary_emitter_append_bytes(object->emitter, object->rodata_section,
                                     str, strlen(str) + 1, &offset) ||
        !binary_emitter_define_symbol(object->emitter, symbol,
                                      BINARY_SYMBOL_LOCAL,
                                      object->rodata_section, offset,
                                      strlen(str) + 1)) {
      e->error = 1;
      return;
    }
    emit_symbol_address(e, object, dest, symbol);
  } else {
    int past = arm64_new_label(e);
    arm64_emit_b(e, past);
    size_t soff = arm64_here(e);
    arm64_emit_bytes(e, str, strlen(str) + 1);
    arm64_bind_label(e, past);
    emit_imm(e, dest, (uint64_t)ELF_BASE + ELF_HDRS + soff);
  }
}

static const IRModuleSymbol *module_function(const IRProgram *program,
                                             const char *name);

/* Emit a bl to an external (or not-yet-seen) symbol through a CALL26
 * relocation, declaring the symbol on first use. Object emission only. */
static int emit_external_call(Arm64Emit *e, Arm64ObjectContext *object,
                              const IRProgram *prog, const char *name) {
  const IRModuleSymbol *callee = module_function(prog, name);
  const char *link_name = callee ? module_link_name(callee) : name;
  if (!binary_emitter_find_symbol(object->emitter, link_name) &&
      !binary_emitter_declare_external(object->emitter, link_name)) {
    e->error = 1;
    return 0;
  }
  size_t call_offset = arm64_here(e);
  arm64_emit_word(e, arm64_bl(0));
  return object_add_relocation(e, object, call_offset,
                               BINARY_RELOCATION_ARM64_CALL26, link_name);
}

/* Load an IR value operand (temp/local/int/float/string) into `dest` as raw
 * 64-bit bits (a float operand yields its IEEE pattern; a string literal
 * yields its address). */
static Arm64Reg load_into(Arm64Emit *e, SlotMap *s, const IROperand *op,
                          Arm64Reg dest) {
  switch (op->kind) {
  case IR_OPERAND_INT:
    emit_imm(e, dest, (uint64_t)op->int_value);
    return dest;
  case IR_OPERAND_FLOAT:
    emit_imm(e, dest, ieee_bits(op));
    return dest;
  case IR_OPERAND_STRING:
    emit_string_literal_address(e, s->object, op->name, dest);
    return dest;
  case IR_OPERAND_TEMP:
  case IR_OPERAND_SYMBOL: {
    int off = slot_find(s, op->name);
    const IRModuleSymbol *global =
        off < 0 && op->kind == IR_OPERAND_SYMBOL ? module_variable(s, op->name)
                                                 : NULL;
    if (global) {
      const char *link_name = module_link_name(global);
      emit_symbol_address(e, s->object, ARM64_X16, link_name);
      emit_load_sized(e, dest, ARM64_X16, module_type_size(global->type),
                      module_type_signed(global->type));
      return dest;
    }
    if (off < 0) off = slot_off(s, op->name);
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
  int off = dst && dst->name ? slot_find(s, dst->name) : -1;
  const IRModuleSymbol *global =
      off < 0 && dst && dst->kind == IR_OPERAND_SYMBOL
          ? module_variable(s, dst->name)
          : NULL;
  if (global) {
    emit_symbol_address(e, s->object, ARM64_X16, module_link_name(global));
    emit_store_sized(e, src, ARM64_X16, module_type_size(global->type));
    return;
  }
  if (off < 0) off = slot_off(s, dst->name);
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

static const IRModuleSymbol *module_function(const IRProgram *program,
                                             const char *name) {
  if (!program || !name) return NULL;
  const IRModuleSymbol *symbol = ir_program_lookup_symbol(program, name);
  return symbol && symbol->kind == IR_MODSYM_FUNCTION ? symbol : NULL;
}

static int call_arg_is_float(const IRProgram *program,
                             const IRInstruction *call, size_t index,
                             const StrSet *floats) {
  const IRModuleSymbol *callee = module_function(program, call->text);
  if (callee && index < callee->param_count && callee->param_types &&
      callee->param_types[index]) {
    return mtlc_type_is_float(callee->param_types[index]);
  }
  return operand_is_float(floats, &call->arguments[index]);
}

static int call_arg_float_bits(const IRProgram *program,
                               const IRInstruction *call, size_t index) {
  const IRModuleSymbol *callee = module_function(program, call->text);
  if (callee && index < callee->param_count && callee->param_types &&
      callee->param_types[index]) {
    return callee->param_types[index]->kind == MTLC_TYPE_FLOAT32 ? 32 : 64;
  }
  return call->arguments[index].float_bits == 32 ? 32 : 64;
}

static int call_returns_float(const IRProgram *program,
                              const IRInstruction *call, const int *retf) {
  const IRModuleSymbol *callee = module_function(program, call->text);
  if (callee && callee->return_type) {
    return mtlc_type_is_float(callee->return_type);
  }
  int index = retf ? prog_fn_index(program, call->text) : -1;
  return index >= 0 && retf[index];
}

static int call_return_float_bits(const IRProgram *program,
                                  const IRInstruction *call) {
  const IRModuleSymbol *callee = module_function(program, call->text);
  if (callee && callee->return_type) {
    return callee->return_type->kind == MTLC_TYPE_FLOAT32 ? 32 : 64;
  }
  return call->dest.float_bits == 32 ? 32 : 64;
}

static int max_outgoing_stack(Arm64Emit *e, const IRFunction *fn,
                              const IRProgram *program,
                              const StrSet *floats) {
  int maximum = 0;
  for (size_t i = 0; i < fn->instruction_count; i++) {
    const IRInstruction *call = &fn->instructions[i];
    if (call->op != IR_OP_CALL || !call->text ||
        strcmp(call->text, "cstr") == 0 || call->argument_count == 0) {
      continue;
    }
    if (call->argument_count > (size_t)INT32_MAX) {
      e->error = 1;
      return 0;
    }
    int count = (int)call->argument_count;
    int *is_float = malloc((size_t)count * sizeof(*is_float));
    Arm64ArgLocation *locations =
        malloc((size_t)count * sizeof(*locations));
    if (!is_float || !locations) {
      free(is_float);
      free(locations);
      e->error = 1;
      return 0;
    }
    for (int k = 0; k < count; k++) {
      is_float[k] = call_arg_is_float(program, call, (size_t)k, floats);
    }
    int bytes = 0;
    if (!arm64_compute_arg_layout(is_float, count, locations, &bytes)) {
      e->error = 1;
    }
    if (bytes > maximum) maximum = bytes;
    free(is_float);
    free(locations);
    if (e->error) return 0;
  }
  return (maximum + 15) & ~15;
}

/* Lower one function body. `fns` maps callee names to entry labels so IR_OP_CALL
 * can resolve a cross-function bl; `prog`/`retf` drive the float ABI (which
 * callees return floats). All may be NULL for the single-function path. */
static int encode_function(Arm64Emit *e, const IRFunction *fn, LblMap *fns,
                           const IRProgram *prog, const int *retf,
                           Arm64ObjectContext *object) {
  SlotMap slots = {0};
  LblMap labels = {0};
  StrSet fs = {0};
  build_float_set(fn, prog, retf, &fs);
  slots.object = object;
  slots.frame = max_outgoing_stack(e, fn, prog, &fs);

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
      if (ops[k]->kind == IR_OPERAND_TEMP ||
          (ops[k]->kind == IR_OPERAND_SYMBOL &&
           !module_variable(&slots, ops[k]->name))) {
        slot_off(&slots, ops[k]->name);
      }
    }
    for (size_t k = 0; k < in->argument_count; k++) {
      const IROperand *a = &in->arguments[k];
      if (a->kind == IR_OPERAND_TEMP ||
          (a->kind == IR_OPERAND_SYMBOL &&
           !module_variable(&slots, a->name))) {
        slot_off(&slots, a->name);
      }
    }
  }

  int frame = (slots.frame + 15) & ~15;
  if (e->error || !arm64_emit_prologue(e, frame, NULL, 0)) {
    goto done;
  }
  /* Home incoming parameters using the same AAPCS64 classifier as calls.
   * Stack arguments begin at caller SP, which is [x29,#16] after our saved
   * FP/LR pair and remains stable regardless of the local frame size. */
  if (fn->parameter_count > 0) {
    int count = (int)fn->parameter_count;
    int *is_float = malloc((size_t)count * sizeof(*is_float));
    Arm64ArgLocation *locations =
        malloc((size_t)count * sizeof(*locations));
    if (!is_float || !locations) {
      free(is_float);
      free(locations);
      e->error = 1;
      goto done;
    }
    for (int i = 0; i < count; i++) {
      const char *type = fn->parameter_types ? fn->parameter_types[i] : NULL;
      is_float[i] = type && strstr(type, "float") != NULL;
    }
    if (!arm64_compute_arg_layout(is_float, count, locations, NULL)) {
      e->error = 1;
    }
    for (int i = 0; i < count && !e->error; i++) {
      int off = slot_off(&slots, fn->parameter_names[i]);
      Arm64ArgLocation location = locations[i];
      if (location.kind == ARM64_ARG_IN_VEC_REGISTER) {
        const char *type =
            fn->parameter_types ? fn->parameter_types[i] : NULL;
        int is_double = !type || !strstr(type, "32");
        arm64_emit_word(e, arm64_str_fp(is_double, (int)location.reg,
                                        ARM64_SP, off));
      } else if (location.kind == ARM64_ARG_IN_GP_REGISTER) {
        arm64_emit_word(e,
                        arm64_str_imm(1, location.reg, ARM64_SP, off));
      } else {
        arm64_emit_word(e, arm64_ldr_imm(1, R_LHS, ARM64_X29,
                                         16 + location.stack_offset));
        arm64_emit_word(e, arm64_str_imm(1, R_LHS, ARM64_SP, off));
      }
    }
    free(is_float);
    free(locations);
    if (e->error) goto done;
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
      const IRModuleSymbol *global =
          slot_find(&slots, in->lhs.name) < 0
              ? module_variable(&slots, in->lhs.name)
              : NULL;
      if (global) {
        emit_symbol_address(e, object, R_RES, module_link_name(global));
      } else {
        emit_lea_local(e, R_RES, slot_off(&slots, in->lhs.name));
      }
      store_dest(e, &slots, &in->dest, R_RES);
      break;
    }
    case IR_OP_LOAD: {
      int size = in->rhs.kind == IR_OPERAND_INT ? (int)in->rhs.int_value : 8;
      Arm64Reg addr = load_into(e, &slots, &in->lhs, R_LHS);
      emit_load_sized(e, R_RES, addr, size,
                      !in->is_unsigned && !in->is_float);
      store_dest(e, &slots, &in->dest, R_RES);
      break;
    }
    case IR_OP_STORE: {
      int size = in->rhs.kind == IR_OPERAND_INT ? (int)in->rhs.int_value : 8;
      Arm64Reg addr = load_into(e, &slots, &in->dest, R_LHS);
      Arm64Reg val = load_into(e, &slots, &in->lhs, R_RHS);
      emit_store_sized(e, val, addr, size);
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
        if (object) {
          char symbol[64];
          size_t offset = 0;
          snprintf(symbol, sizeof(symbol), ".Lmtlc.str.%u",
                   object->string_id++);
          if (!binary_emitter_append_bytes(object->emitter,
                                           object->rodata_section, str,
                                           strlen(str) + 1, &offset) ||
              !binary_emitter_define_symbol(
                  object->emitter, symbol, BINARY_SYMBOL_LOCAL,
                  object->rodata_section, offset, strlen(str) + 1)) {
            e->error = 1;
            break;
          }
          emit_symbol_address(e, object, R_RES, symbol);
        } else {
          int past = arm64_new_label(e);
          arm64_emit_b(e, past);
          size_t soff = arm64_here(e);
          arm64_emit_bytes(e, str, strlen(str) + 1);
          arm64_bind_label(e, past);
          emit_imm(e, R_RES, (uint64_t)ELF_BASE + ELF_HDRS + soff);
        }
        if (in->dest.kind == IR_OPERAND_TEMP ||
            in->dest.kind == IR_OPERAND_SYMBOL) {
          store_dest(e, &slots, &in->dest, R_RES);
        }
        break;
      }
      /* Runtime traps: a plain relocatable object may only assume libc
       * (matching the x86 backend without stack-trace support), so
       * mettle_crash_trap[_ex] lowers to puts(message); exit(1). The legacy
       * self-contained product has no libc and exits via the raw syscall. */
      if (strcmp(in->text, "mettle_crash_trap_ex") == 0 ||
          strcmp(in->text, "mettle_crash_trap") == 0) {
        size_t msg_index =
            strcmp(in->text, "mettle_crash_trap_ex") == 0 ? 1u : 0u;
        if (object) {
          if (in->argument_count > msg_index) {
            load_into(e, &slots, &in->arguments[msg_index], ARM64_X0);
          } else {
            emit_string_literal_address(e, object, "Fatal error", ARM64_X0);
          }
          if (!emit_external_call(e, object, prog, "puts")) break;
          emit_imm(e, ARM64_X0, 1);
          if (!emit_external_call(e, object, prog, "exit")) break;
        } else {
          emit_imm(e, ARM64_X0, 1);
          arm64_emit_word(e, arm64_movz(1, ARM64_X8, 93, 0)); /* exit */
          arm64_emit_word(e, 0xD4000001u);                    /* svc #0 */
        }
        break;
      }
      /* Marshal through one AAPCS64 layout. GP and FP registers are independent
       * banks; overflow values occupy the frame's reserved outgoing-call area
       * at [sp,#stack_offset]. This is required for cuLaunchKernel's 11-argument
       * C ABI, not merely for synthetic many-argument tests. */
      if (in->argument_count > 0) {
        int count = (int)in->argument_count;
        int *is_float = malloc((size_t)count * sizeof(*is_float));
        Arm64ArgLocation *locations =
            malloc((size_t)count * sizeof(*locations));
        if (!is_float || !locations) {
          free(is_float);
          free(locations);
          e->error = 1;
          break;
        }
        for (int k = 0; k < count; k++) {
          is_float[k] = call_arg_is_float(prog, in, (size_t)k, &fs);
        }
        if (!arm64_compute_arg_layout(is_float, count, locations, NULL)) {
          e->error = 1;
        }
        for (int k = 0; k < count && !e->error; k++) {
          const IROperand *arg = &in->arguments[k];
          Arm64ArgLocation location = locations[k];
          if (location.kind == ARM64_ARG_IN_VEC_REGISTER) {
            int is_double =
                call_arg_float_bits(prog, in, (size_t)k) != 32;
            arm64_emit_word(
                e, arm64_fmov_gp(is_double, (int)location.reg,
                                 load_into(e, &slots, arg, R_LHS)));
          } else if (location.kind == ARM64_ARG_IN_GP_REGISTER) {
            load_into(e, &slots, arg, location.reg);
          } else {
            load_into(e, &slots, arg, R_LHS);
            arm64_emit_word(e, arm64_str_imm(1, R_LHS, ARM64_SP,
                                             location.stack_offset));
          }
        }
        free(is_float);
        free(locations);
        if (e->error) break;
      }

      if (prog_fn_index(prog, in->text) >= 0) {
        arm64_emit_bl(e, label_for(e, fns, in->text));
      } else if (object) {
        if (!emit_external_call(e, object, prog, in->text)) {
          break;
        }
      } else {
        e->error = 1;
        break;
      }
      if (in->dest.kind == IR_OPERAND_TEMP ||
          in->dest.kind == IR_OPERAND_SYMBOL) {
        int resf = call_returns_float(prog, in, retf) ||
                   set_has(&fs, in->dest.name);
        if (resf) { /* float result arrives in d0/s0 */
          int d = call_return_float_bits(prog, in) != 32;
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
          const IRModuleSymbol *current = module_function(prog, fn->name);
          int d = current && current->return_type
                      ? current->return_type->kind != MTLC_TYPE_FLOAT32
                      : in->lhs.float_bits != 32;
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
  return encode_function(e, fn, NULL, NULL, NULL, NULL);
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
    } else if (!encode_function(e, fn, &fns, prog, retf, NULL)) {
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

static void arm64_object_error(char *error, size_t capacity,
                               const char *message) {
  if (error && capacity > 0) {
    snprintf(error, capacity, "%s", message ? message : "unknown error");
  }
}

/* Emit a global struct or array. With a folded initializer image it goes to
 * .data followed by one ADDR64 relocation per pointer-sized hole (a function's
 * address, another global's address, or a string element, whose characters and
 * { chars, length } record are parked in .rodata first). Without one it is
 * plain zeroed .bss. Mirrors the x86-64 path in globals.c. */
static int arm64_object_emit_aggregate(Arm64ObjectContext *object,
                                       const IRModuleSymbol *symbol,
                                       const char *link_name,
                                       size_t data_section,
                                       size_t bss_section) {
  BinaryEmitter *emitter = object->emitter;
  size_t size = mtlc_type_size(symbol->type);
  size_t alignment = symbol->type->alignment ? symbol->type->alignment : 8;
  size_t offset = 0;

  if (size == 0) return 0;

  if (!symbol->init_bytes || symbol->init_bytes_size == 0) {
    if (!binary_emitter_align_section(emitter, bss_section, alignment, 0) ||
        !binary_emitter_append_zeros(emitter, bss_section, size, &offset)) {
      return 0;
    }
    return binary_emitter_define_symbol(emitter, link_name,
                                        BINARY_SYMBOL_GLOBAL, bss_section,
                                        offset, size);
  }

  if (!binary_emitter_align_section(emitter, data_section, alignment, 0) ||
      !binary_emitter_append_bytes(emitter, data_section, symbol->init_bytes,
                                   symbol->init_bytes_size, &offset)) {
    return 0;
  }

  for (size_t i = 0; i < symbol->init_reloc_count; i++) {
    const IRInitReloc *reloc = &symbol->init_relocs[i];
    const char *target = reloc->symbol;
    char chars_symbol[64];
    char record_symbol[64];

    if (!target) {
      size_t chars_offset = 0;
      size_t length = reloc->string ? strlen(reloc->string) : 0;
      snprintf(chars_symbol, sizeof(chars_symbol), ".Lmtlc.gstr.%u",
               object->string_id++);
      if (!binary_emitter_append_bytes(emitter, object->rodata_section,
                                       reloc->string ? reloc->string : "",
                                       length + 1, &chars_offset) ||
          !binary_emitter_define_symbol(emitter, chars_symbol,
                                        BINARY_SYMBOL_LOCAL,
                                        object->rodata_section, chars_offset,
                                        length + 1)) {
        return 0;
      }
      target = chars_symbol;

      if (reloc->string_wants_record) {
        /* A `string` slot points at a { chars, length } record, not at the
         * characters, which is what a string value is everywhere else. */
        size_t record_offset = 0;
        uint64_t encoded_length = (uint64_t)length;
        BinarySection *rodata = NULL;
        snprintf(record_symbol, sizeof(record_symbol), ".Lmtlc.gstr.%u",
                 object->string_id++);
        if (!binary_emitter_align_section(emitter, object->rodata_section, 8,
                                          0) ||
            !binary_emitter_append_zeros(emitter, object->rodata_section, 16,
                                         &record_offset) ||
            !binary_emitter_define_symbol(emitter, record_symbol,
                                          BINARY_SYMBOL_LOCAL,
                                          object->rodata_section,
                                          record_offset, 16) ||
            !binary_emitter_add_relocation(emitter, object->rodata_section,
                                           record_offset,
                                           BINARY_RELOCATION_ADDR64,
                                           chars_symbol, 0)) {
          return 0;
        }
        rodata = binary_emitter_get_section(emitter, object->rodata_section);
        if (!rodata || !rodata->data ||
            record_offset + 16 > rodata->size) {
          return 0;
        }
        memcpy(rodata->data + record_offset + 8, &encoded_length, 8);
        target = record_symbol;
      }
    } else {
      const IRModuleSymbol *referenced =
          object->program ? ir_program_lookup_symbol(object->program, target)
                          : NULL;
      if (referenced) {
        target = module_link_name(referenced);
      }
    }

    if (!binary_emitter_add_relocation(emitter, data_section,
                                       offset + reloc->offset,
                                       BINARY_RELOCATION_ADDR64, target, 0)) {
      return 0;
    }
  }

  return binary_emitter_define_symbol(emitter, link_name, BINARY_SYMBOL_GLOBAL,
                                      data_section, offset,
                                      symbol->init_bytes_size);
}

static int arm64_object_emit_global(Arm64ObjectContext *object,
                                    const IRModuleSymbol *symbol,
                                    size_t data_section,
                                    size_t bss_section) {
  BinaryEmitter *emitter = object->emitter;
  const char *link_name = module_link_name(symbol);
  if (!link_name || !link_name[0]) return 0;
  if (symbol->is_extern) {
    return binary_emitter_declare_external(emitter, link_name);
  }
  if (!symbol->type || symbol->has_unfoldable_initializer) return 0;

  /* Mettle strings are a {chars,length} pair in host memory. */
  if (symbol->type->kind == MTLC_TYPE_STRING) {
    size_t value_offset = 0;
    if (!binary_emitter_align_section(emitter, data_section, 8, 0) ||
        !binary_emitter_append_zeros(emitter, data_section, 16,
                                     &value_offset)) {
      return 0;
    }
    if (symbol->has_initializer && symbol->init_string) {
      char chars_symbol[64];
      size_t chars_offset = 0;
      size_t length = strlen(symbol->init_string);
      snprintf(chars_symbol, sizeof(chars_symbol), ".Lmtlc.gstr.%u",
               object->string_id++);
      if (!binary_emitter_append_bytes(
              emitter, object->rodata_section, symbol->init_string,
              length + 1, &chars_offset) ||
          !binary_emitter_define_symbol(
              emitter, chars_symbol, BINARY_SYMBOL_LOCAL,
              object->rodata_section, chars_offset, length + 1)) {
        return 0;
      }
      BinarySection *data =
          binary_emitter_get_section(emitter, data_section);
      uint64_t encoded_length = (uint64_t)length;
      if (!data || value_offset + 16 > data->size) return 0;
      memcpy(data->data + value_offset + 8, &encoded_length, 8);
      if (!binary_emitter_add_relocation(
              emitter, data_section, value_offset, BINARY_RELOCATION_ADDR64,
              chars_symbol, 0)) {
        return 0;
      }
    }
    return binary_emitter_define_symbol(
        emitter, link_name, BINARY_SYMBOL_GLOBAL, data_section, value_offset,
        16);
  }

  if (symbol->type->kind == MTLC_TYPE_STRUCT ||
      symbol->type->kind == MTLC_TYPE_ARRAY) {
    return arm64_object_emit_aggregate(object, symbol, link_name, data_section,
                                       bss_section);
  }

  size_t size = mtlc_type_size(symbol->type);
  if (size == 0 || size > 8) return 0;
  size_t alignment = symbol->type->alignment ? symbol->type->alignment : size;
  size_t section = symbol->has_initializer ? data_section : bss_section;
  size_t offset = 0;
  if (!binary_emitter_align_section(emitter, section, alignment, 0)) return 0;
  if (symbol->has_initializer) {
    unsigned char bytes[8] = {0};
    if (symbol->init_is_float) {
      double value = 0.0;
      memcpy(&value, &symbol->init_bits, 8);
      if (symbol->type->kind == MTLC_TYPE_FLOAT32) {
        float narrowed = (float)value;
        memcpy(bytes, &narrowed, 4);
      } else if (symbol->type->kind == MTLC_TYPE_FLOAT64) {
        memcpy(bytes, &value, 8);
      } else {
        return 0;
      }
    } else {
      uint64_t bits = (uint64_t)symbol->init_bits;
      memcpy(bytes, &bits, size);
    }
    if (!binary_emitter_append_bytes(emitter, section, bytes, size, &offset))
      return 0;
  } else if (!binary_emitter_append_zeros(emitter, section, size, &offset)) {
    return 0;
  }
  return binary_emitter_define_symbol(emitter, link_name,
                                      BINARY_SYMBOL_GLOBAL, section, offset,
                                      size);
}

int arm64_ir_write_object(const IRProgram *prog, const char *path, char *error,
                          size_t error_capacity) {
  BinaryEmitter *emitter = NULL;
  Arm64Emit code;
  LblMap functions = {0};
  int *returns_float = NULL;
  int success = 0;
  int code_initialized = 0;

  if (!prog || !path || !path[0]) {
    arm64_object_error(error, error_capacity, "invalid AArch64 object input");
    return 0;
  }
  emitter = binary_emitter_create(BINARY_TARGET_FORMAT_ELF_ARM64);
  if (!emitter) {
    arm64_object_error(error, error_capacity,
                       "out of memory creating AArch64 object emitter");
    return 0;
  }

  Arm64ObjectContext object = {0};
  object.emitter = emitter;
  object.program = prog;
  object.text_section = binary_emitter_get_or_create_section(
      emitter, ".text", BINARY_SECTION_TEXT, 0, 4);
  object.rodata_section = binary_emitter_get_or_create_section(
      emitter, ".rodata", BINARY_SECTION_RDATA, 0, 8);
  size_t data_section = binary_emitter_get_or_create_section(
      emitter, ".data", BINARY_SECTION_DATA, 0, 8);
  size_t bss_section = binary_emitter_get_or_create_section(
      emitter, ".bss", BINARY_SECTION_BSS, 0, 8);
  if (object.text_section == (size_t)-1 ||
      object.rodata_section == (size_t)-1 || data_section == (size_t)-1 ||
      bss_section == (size_t)-1) {
    arm64_object_error(error, error_capacity,
                       binary_emitter_get_error(emitter));
    goto cleanup;
  }

  /* Undefined symbols must exist before relocations reference them; globals
   * are laid out before code so address materialization is uniformly symbolic. */
  for (size_t i = 0; i < prog->module_symbol_count; i++) {
    const IRModuleSymbol *symbol = &prog->module_symbols[i];
    if (symbol->kind == IR_MODSYM_VARIABLE) {
      if (!arm64_object_emit_global(&object, symbol, data_section,
                                    bss_section)) {
        arm64_object_error(error, error_capacity,
                           binary_emitter_get_error(emitter)
                               ? binary_emitter_get_error(emitter)
                               : "unsupported AArch64 global variable");
        goto cleanup;
      }
    } else if (symbol->kind == IR_MODSYM_FUNCTION && symbol->is_extern) {
      if (!binary_emitter_declare_external(emitter,
                                           module_link_name(symbol))) {
        arm64_object_error(error, error_capacity,
                           binary_emitter_get_error(emitter));
        goto cleanup;
      }
    }
  }

  returns_float = calloc(prog->function_count ? prog->function_count : 1,
                         sizeof(*returns_float));
  if (!returns_float) {
    arm64_object_error(error, error_capacity,
                       "out of memory classifying AArch64 functions");
    goto cleanup;
  }
  for (size_t iteration = 0; iteration <= prog->function_count; iteration++) {
    int changed = 0;
    for (size_t i = 0; i < prog->function_count; i++) {
      if (!returns_float[i] &&
          fn_returns_float(prog->functions[i], prog, returns_float)) {
        returns_float[i] = 1;
        changed = 1;
      }
    }
    if (!changed) break;
  }

  arm64_emit_init(&code);
  code_initialized = 1;
  if (!binary_emitter_define_symbol(emitter, "$x", BINARY_SYMBOL_LOCAL,
                                    object.text_section, 0, 0)) {
    arm64_object_error(error, error_capacity,
                       binary_emitter_get_error(emitter));
    goto cleanup;
  }

  for (size_t i = 0; i < prog->function_count && !code.error; i++) {
    const IRFunction *function = prog->functions[i];
    const IRModuleSymbol *symbol =
        module_function(prog, function ? function->name : NULL);
    if (!function || function->is_kernel ||
        (symbol && (symbol->is_extern || !symbol->has_body)) ||
        strcmp(function->name, "cstr") == 0) {
      continue;
    }
    size_t start = arm64_here(&code);
    arm64_bind_label(&code,
                     label_for(&code, &functions, function->name));
    if (!encode_function(&code, function, &functions, prog, returns_float,
                         &object)) {
      break;
    }
    const char *link_name = symbol ? module_link_name(symbol) : function->name;
    if (!binary_emitter_define_symbol(
            emitter, link_name, BINARY_SYMBOL_GLOBAL, object.text_section,
            start, arm64_here(&code) - start)) {
      code.error = 1;
      break;
    }
  }
  if (code.error || !arm64_emit_finalize(&code)) {
    arm64_object_error(error, error_capacity,
                       binary_emitter_get_error(emitter)
                           ? binary_emitter_get_error(emitter)
                           : "AArch64 IR lowering failed");
    goto cleanup;
  }
  if (!binary_emitter_append_bytes(emitter, object.text_section,
                                   code.code.data, code.code.len, NULL) ||
      !binary_emitter_write_object_file(emitter, path)) {
    arm64_object_error(error, error_capacity,
                       binary_emitter_get_error(emitter));
    goto cleanup;
  }
  success = 1;

cleanup:
  if (code_initialized) arm64_emit_free(&code);
  free(functions.names);
  free(functions.ids);
  free(returns_float);
  binary_emitter_destroy(emitter);
  return success;
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
