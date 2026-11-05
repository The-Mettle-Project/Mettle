#include "codegen/binary/arm64_ir.h"
#include "codegen/binary/strength_rules.h"
#include "codegen/binary_emitter.h"
#include "codegen/binary_emitter_internal.h"
#include "codegen/target.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ELF_BASE 0x400000u
#define ELF_HDRS 176u

#define ELF_DATA_VADDR 0x40000000u
#define ELF_TEXT_LIMIT (ELF_DATA_VADDR - ELF_BASE)

#define HEAP_CURSOR_OFF 0u
#define HEAP_END_OFF 8u
#define DATA_RESERVED 16u

#define SYS_WRITE 64
#define SYS_EXIT 93
#define SYS_MMAP 222
#define ARM64_SVC0 0xD4000001u
#define HEAP_ARENA_BYTES 0x4000000u

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

typedef struct {
  unsigned char *bytes;
  size_t len;
  size_t cap;
  const char **names;
  size_t *offs;
  int count;
  int name_cap;
  const char **fixup_fns;
  size_t *fixup_offs;
  int fixup_count;
  int fixup_cap;
} Arm64Data;

typedef struct {
  const char **names;
  int *offs;
  int count;
  int cap;
  int frame;
  Arm64ObjectContext *object;
  const IRProgram *program;
  Arm64Data *data;
  struct LblMap *fns;
  const struct NarrowMap *narrow;
  const struct AggMap *aggregates;
} SlotMap;

static const IRModuleSymbol *module_variable(const SlotMap *slots,
                                             const char *name) {
  if (!slots || !slots->program || !name) return NULL;
  const IRModuleSymbol *symbol =
      ir_program_lookup_symbol(slots->program, name);
  return symbol && symbol->kind == IR_MODSYM_VARIABLE ? symbol : NULL;
}

typedef enum {
  STUB_MALLOC, STUB_CALLOC, STUB_FREE, STUB_PUTS, STUB_PUTCHAR, STUB_WRITE,
  STUB_STRLEN, STUB_MEMCPY, STUB_MEMMOVE, STUB_MEMSET, STUB_EXIT, STUB_ABORT,
  STUB_STR_FROM_INT, STUB_STR_FROM_UINT, STUB_STR_FROM_BOOL,
  STUB_STR_FROM_CHAR, STUB_STR_EQ, STUB_STR_FREE, STUB_REGION_BEGIN,
  STUB_REGION_END,
  STUB_COUNT
} Arm64StubId;

static const struct {
  const char *name;
  int needs_malloc;
} RUNTIME_STUBS[STUB_COUNT] = {
    {"malloc", 0},  {"calloc", 1},  {"free", 0},    {"puts", 0},
    {"putchar", 0}, {"write", 0},   {"strlen", 0},  {"memcpy", 0},
    {"memmove", 0}, {"memset", 0},  {"exit", 0},    {"abort", 0},
    {"mettle_string_from_int", 1},  {"mettle_string_from_uint", 1},
    {"mettle_string_from_bool", 1},
    {"mettle_string_from_char", 1},
    {"mettle_string_eq", 0},
    {"mettle_string_free", 0},
    {"mettle_safety_region_begin", 0},
    {"mettle_safety_region_end", 0},
};

static int runtime_stub_index(const char *name) {
  if (!name) return -1;
  if (strcmp(name, "mettle_heap_zeroed") == 0) return STUB_MALLOC;
  for (int i = 0; i < STUB_COUNT; i++) {
    if (strcmp(name, RUNTIME_STUBS[i].name) == 0) return i;
  }
  return -1;
}

static long data_offset_of(const Arm64Data *d, const char *name) {
  if (!d || !name) return -1;
  for (int i = 0; i < d->count; i++) {
    if (d->names[i] == name || strcmp(d->names[i], name) == 0) {
      return (long)d->offs[i];
    }
  }
  return -1;
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
    arm64_fail(e, "could not relocate a reference to '%s'",
               symbol ? symbol : "(null)");
    return 0;
  }
  return 1;
}

static void emit_imm(Arm64Emit *e, Arm64Reg rd, uint64_t v);
static int label_for(Arm64Emit *e, struct LblMap *m, const char *name);
static int prog_fn_index(const IRProgram *prog, const char *name);
static void emit_lea_local(Arm64Emit *e, Arm64Reg rd, int off);
static void emit_slot_ldr(Arm64Emit *e, Arm64Reg rt, int off);
static void emit_slot_str(Arm64Emit *e, Arm64Reg rt, int off);
static size_t data_reserve(Arm64Data *d, size_t n, size_t align);
static int io_stub_intrinsic(const char *name, int *with_newline,
                             int *is_string);
static const IRModuleSymbol *module_function(const IRProgram *program,
                                             const char *name);

static void emit_symbol_address(Arm64Emit *e, Arm64ObjectContext *object,
                                Arm64Reg rd, const char *symbol) {
  size_t at = arm64_here(e);
  arm64_emit_word(e, 0x90000000u | (uint32_t)rd);
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

static void emit_global_address(Arm64Emit *e, const SlotMap *s, Arm64Reg rd,
                                const IRModuleSymbol *global) {
  const char *link_name = module_link_name(global);
  if (s->object) {
    emit_symbol_address(e, s->object, rd, link_name);
    return;
  }
  long off = data_offset_of(s->data, link_name);
  if (off < 0 && global) off = data_offset_of(s->data, global->name);
  if (off < 0) {
    arm64_fail(e, "global '%s' has no slot in the data image",
               link_name ? link_name : "(unnamed)");
    return;
  }
  emit_imm(e, rd, (uint64_t)ELF_DATA_VADDR + (uint64_t)off);
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
  case 1:
    arm64_emit_word(e, arm64_ldrb_imm(dest, address, 0));
    if (sign_extend) arm64_emit_word(e, arm64_sxtb(dest, dest));
    break;
  default:
    arm64_fail(e, "no %d-byte scalar load", size);
    break;
  }
}

static void emit_store_sized(Arm64Emit *e, Arm64Reg source,
                             Arm64Reg address, int size) {
  switch (size) {
  case 8: arm64_emit_word(e, arm64_str_imm(1, source, address, 0)); break;
  case 4: arm64_emit_word(e, arm64_str_imm(0, source, address, 0)); break;
  case 2: arm64_emit_word(e, arm64_strh_imm(source, address, 0)); break;
  case 1: arm64_emit_word(e, arm64_strb_imm(source, address, 0)); break;
  default:
    arm64_fail(e, "no %d-byte scalar store", size);
    break;
  }
}

static void emit_f16_widen(Arm64Emit *e, Arm64Reg v) {
  arm64_emit_word(e, arm64_fmov_gp(0, 0, v));
  arm64_emit_word(e, arm64_fcvt_h2s(0, 0));
  arm64_emit_word(e, arm64_fmov_to_gp(0, v, 0));
}

static void emit_f16_narrow(Arm64Emit *e, Arm64Reg v) {
  arm64_emit_word(e, arm64_fmov_gp(0, 0, v));
  arm64_emit_word(e, arm64_fcvt_s2h(0, 0));
  arm64_emit_word(e, arm64_fmov_to_gp(0, v, 0));
}

static void emit_bf16_widen(Arm64Emit *e, Arm64Reg v) {
  arm64_emit_word(e, arm64_lsl_imm(0, v, v, 16));
}

static void emit_bf16_narrow(Arm64Emit *e, Arm64Reg v, Arm64Reg t1,
                             Arm64Reg t2) {
  arm64_emit_word(e, arm64_lsl_imm(0, t2, v, 1));
  arm64_emit_word(e, arm64_movz(0, t1, 0xFF00, 1));
  arm64_emit_word(e, arm64_cmp_reg(0, t2, t1));
  arm64_emit_word(e, arm64_lsl_imm(0, t1, v, 15));
  arm64_emit_word(e, arm64_lsr_imm(0, t1, t1, 31));
  arm64_emit_word(e, arm64_movz(0, t2, 0x7FFF, 0));
  arm64_emit_word(e, arm64_add_reg(0, t1, t1, t2));
  arm64_emit_word(e, arm64_add_reg(0, t1, t1, v));
  arm64_emit_word(e, arm64_lsr_imm(0, t1, t1, 16));
  arm64_emit_word(e, arm64_lsr_imm(0, t2, v, 23));
  arm64_emit_word(e, arm64_lsl_imm(0, t2, t2, 7));
  arm64_emit_word(e, arm64_add_imm(0, t2, t2, 64, 0));
  arm64_emit_word(e, arm64_csel(0, v, t2, t1, ARM64_HI));
}

static int small_float_class(const IRInstruction *in) {
  if (in->alias_class == IR_ALIAS_CLASS_F16) return 16;
  if (in->alias_class == IR_ALIAS_CLASS_BF16) return 8;
  return 0;
}

static int cast_small_float_target(const IRInstruction *in) {
  if (!in->text) return 0;
  if (strcmp(in->text, "float16") == 0) return 16;
  if (strcmp(in->text, "bfloat16") == 0) return 8;
  return 0;
}

static const char *type_text_base(const char *t, char *buffer, size_t size) {
  size_t n = 0;
  if (!t || size == 0) {
    if (size > 0) buffer[0] = '\0';
    return buffer;
  }
  while (t[n] && t[n] != '*' && t[n] != '[' && t[n] != ' ' && n + 1 < size) {
    n++;
  }
  memcpy(buffer, t, n);
  buffer[n] = '\0';
  return buffer;
}

static int type_text_base_is(const char *t, const char *name) {
  char base[64];
  return strcmp(type_text_base(t, base, sizeof(base)), name) == 0;
}

static int type_elem_size(const char *t) {
  char base[64];
  if (!t) return 8;
  if (strchr(t, '*')) return 8;
  type_text_base(t, base, sizeof(base));
  if (strcmp(base, "int64") == 0 || strcmp(base, "uint64") == 0 ||
      strcmp(base, "float64") == 0) {
    return 8;
  }
  if (strcmp(base, "int32") == 0 || strcmp(base, "uint32") == 0 ||
      strcmp(base, "float32") == 0) {
    return 4;
  }
  if (strcmp(base, "int16") == 0 || strcmp(base, "uint16") == 0) return 2;
  if (strcmp(base, "int8") == 0 || strcmp(base, "uint8") == 0 ||
      strcmp(base, "bool") == 0 || strcmp(base, "char") == 0) {
    return 1;
  }
  return 8;
}

static int type_is_aggregate(const MtlcType *t) {
  if (!t) return 0;
  return t->kind == MTLC_TYPE_STRUCT || t->kind == MTLC_TYPE_ARRAY ||
         t->kind == MTLC_TYPE_TAGGED_ENUM || t->kind == MTLC_TYPE_STRING;
}

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

static const MtlcType *declared_type(const IRProgram *prog,
                                     const IRInstruction *in) {
  if (in->value_type) return in->value_type;
  if (prog && in->text && !strchr(in->text, '[') && !strchr(in->text, '*')) {
    return ir_program_lookup_type(prog, in->text);
  }
  return NULL;
}

static int declare_local_size(const IRProgram *prog, const IRInstruction *in) {
  const MtlcType *t = declared_type(prog, in);
  if (t) {
    size_t size = mtlc_type_size(t);
    if (size > 0 && size < (size_t)1 << 24) return (int)size;
  }
  return local_size_bytes(in->text);
}

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

static int slot_off(SlotMap *s, const char *name) {
  return slot_alloc(s, name, 8);
}

static int slot_find(const SlotMap *s, const char *name) {
  for (int i = 0; i < s->count; i++) {
    if (s->names[i] == name || strcmp(s->names[i], name) == 0) {
      return s->offs[i];
    }
  }
  return -1;
}

static int type_text_is_float_scalar(const char *t) {
  if (!t || strchr(t, '*') != NULL || strchr(t, '[') != NULL) return 0;
  return type_text_base_is(t, "float32") || type_text_base_is(t, "float64");
}

static int type_text_float_bits(const char *t) {
  return type_text_base_is(t, "float32") ? 32 : 64;
}

static int darwin_packs_stack_args(void) {
  return mtlc_target()->os == MTLC_TARGET_OS_MACOS;
}

static int stack_size_of_type(const MtlcType *t) {
  if (!t || !darwin_packs_stack_args()) return 8;
  switch (t->kind) {
  case MTLC_TYPE_INT8:
  case MTLC_TYPE_UINT8:
  case MTLC_TYPE_BOOL:
    return 1;
  case MTLC_TYPE_INT16:
  case MTLC_TYPE_UINT16:
  case MTLC_TYPE_FLOAT16:
  case MTLC_TYPE_BFLOAT16:
    return 2;
  case MTLC_TYPE_INT32:
  case MTLC_TYPE_UINT32:
  case MTLC_TYPE_FLOAT32:
    return 4;
  default:
    return 8;
  }
}

static int stack_size_of_type_text(const char *t) {
  if (!t || !darwin_packs_stack_args() || strchr(t, '*') != NULL ||
      strchr(t, '[') != NULL) {
    return 8;
  }
  if (type_text_base_is(t, "int8") || type_text_base_is(t, "uint8") ||
      type_text_base_is(t, "bool")) {
    return 1;
  }
  if (type_text_base_is(t, "int16") || type_text_base_is(t, "uint16") ||
      type_text_base_is(t, "float16") || type_text_base_is(t, "bfloat16")) {
    return 2;
  }
  if (type_text_base_is(t, "int32") || type_text_base_is(t, "uint32") ||
      type_text_base_is(t, "float32")) {
    return 4;
  }
  return 8;
}

static uint32_t stack_store_of_size(int size, Arm64Reg rt, int offset) {
  switch (size) {
  case 1:
    return arm64_strb_imm(rt, ARM64_SP, offset);
  case 2:
    return arm64_strh_imm(rt, ARM64_SP, offset);
  case 4:
    return arm64_str_imm(0, rt, ARM64_SP, offset);
  default:
    return arm64_str_imm(1, rt, ARM64_SP, offset);
  }
}

static uint64_t ieee_bits_at(const IROperand *op, int bits) {
  if (bits == 32) {
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

static uint64_t ieee_bits(const IROperand *op) {
  return ieee_bits_at(op, op->float_bits == 32 ? 32 : 64);
}

typedef struct LblMap {
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
      arm64_fail(e, "out of memory growing the label map");
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

typedef struct StrSet {
  const char **names;
  int *bits;
  int count;
  int cap;
} StrSet;

typedef struct NarrowMap {
  const char **names;
  signed char *sizes;
  signed char *is_signed;
  int count;
  int cap;
} NarrowMap;

static int narrow_size_of(const NarrowMap *m, const char *name, int *is_signed) {
  if (!m || !name) return 0;
  for (int i = 0; i < m->count; i++) {
    if (m->names[i] == name || strcmp(m->names[i], name) == 0) {
      if (is_signed) *is_signed = m->is_signed[i];
      return m->sizes[i];
    }
  }
  return 0;
}

static void narrow_remove(NarrowMap *m, const char *name) {
  if (!name) return;
  for (int i = 0; i < m->count; i++) {
    if (m->names[i] == name || strcmp(m->names[i], name) == 0) {
      m->count--;
      m->names[i] = m->names[m->count];
      m->sizes[i] = m->sizes[m->count];
      m->is_signed[i] = m->is_signed[m->count];
      return;
    }
  }
}

static void narrow_add(NarrowMap *m, const char *name, int size, int sign) {
  if (!name || (size != 1 && size != 2 && size != 4)) return;
  if (narrow_size_of(m, name, NULL)) return;
  if (m->count == m->cap) {
    int cap = m->cap ? m->cap * 2 : 16;
    const char **n = realloc(m->names, (size_t)cap * sizeof(*n));
    signed char *z = realloc(m->sizes, (size_t)cap * sizeof(*z));
    signed char *g = realloc(m->is_signed, (size_t)cap * sizeof(*g));
    if (n) m->names = n;
    if (z) m->sizes = z;
    if (g) m->is_signed = g;
    if (!n || !z || !g) return;
    m->cap = cap;
  }
  m->names[m->count] = name;
  m->sizes[m->count] = (signed char)size;
  m->is_signed[m->count] = (signed char)(sign != 0);
  m->count++;
}

typedef struct AggMap {
  const char **names;
  int *sizes;
  int count;
  int cap;
} AggMap;

static int agg_size_of(const AggMap *m, const char *name) {
  if (!m || !name) return 0;
  for (int i = 0; i < m->count; i++) {
    if (m->names[i] == name || strcmp(m->names[i], name) == 0) {
      return m->sizes[i];
    }
  }
  return 0;
}

static void agg_remove(AggMap *m, const char *name) {
  if (!name) return;
  for (int i = 0; i < m->count; i++) {
    if (m->names[i] == name || strcmp(m->names[i], name) == 0) {
      m->count--;
      m->names[i] = m->names[m->count];
      m->sizes[i] = m->sizes[m->count];
      return;
    }
  }
}

static void agg_add(AggMap *m, const char *name, int size) {
  if (!name || size <= 0 || agg_size_of(m, name)) return;
  if (m->count == m->cap) {
    int cap = m->cap ? m->cap * 2 : 16;
    const char **n = realloc(m->names, (size_t)cap * sizeof(*n));
    int *z = realloc(m->sizes, (size_t)cap * sizeof(*z));
    if (n) m->names = n;
    if (z) m->sizes = z;
    if (!n || !z) return;
    m->cap = cap;
  }
  m->names[m->count] = name;
  m->sizes[m->count] = size;
  m->count++;
}

static int set_has(const StrSet *s, const char *n) {
  if (!n) return 0;
  for (int i = 0; i < s->count; i++) {
    if (s->names[i] == n || strcmp(s->names[i], n) == 0) return 1;
  }
  return 0;
}
static void set_add_bits(StrSet *s, const char *n, int bits) {
  if (!n) return;
  for (int i = 0; i < s->count; i++) {
    if (s->names[i] == n || strcmp(s->names[i], n) == 0) {
      if (bits && !s->bits[i]) s->bits[i] = bits;
      return;
    }
  }
  if (s->count == s->cap) {
    int cap = s->cap ? s->cap * 2 : 32;
    const char **p = realloc(s->names, (size_t)cap * sizeof(*p));
    int *w = realloc(s->bits, (size_t)cap * sizeof(*w));
    if (p) s->names = p;
    if (w) s->bits = w;
    if (!p || !w) return;
    s->cap = cap;
  }
  s->names[s->count] = n;
  s->bits[s->count] = bits;
  s->count++;
}

static void set_add(StrSet *s, const char *n) { set_add_bits(s, n, 0); }

static void set_remove(StrSet *s, const char *n) {
  if (!n) return;
  for (int i = 0; i < s->count; i++) {
    if (s->names[i] == n || strcmp(s->names[i], n) == 0) {
      s->count--;
      s->names[i] = s->names[s->count];
      s->bits[i] = s->bits[s->count];
      return;
    }
  }
}

static void set_rebind(StrSet *s, const char *n, int present, int bits) {
  set_remove(s, n);
  if (present) set_add_bits(s, n, bits);
}

static int set_bits(const StrSet *s, const char *n) {
  if (!n) return 0;
  for (int i = 0; i < s->count; i++) {
    if (s->names[i] == n || strcmp(s->names[i], n) == 0) return s->bits[i];
  }
  return 0;
}

static int float_op_bits(const StrSet *fs, const IROperand *a,
                         const IROperand *b, int instruction_bits);

static int operand_float_bits(const StrSet *fs, const IROperand *op) {
  if (op->float_bits) return op->float_bits;
  if (op->kind == IR_OPERAND_TEMP || op->kind == IR_OPERAND_SYMBOL) {
    int bits = set_bits(fs, op->name);
    if (bits) return bits;
  }
  return 64;
}

static int operand_is_float(const StrSet *fs, const IROperand *op) {
  if (op->kind == IR_OPERAND_FLOAT || op->float_bits != 0) return 1;
  if ((op->kind == IR_OPERAND_TEMP || op->kind == IR_OPERAND_SYMBOL))
    return set_has(fs, op->name);
  return 0;
}

static int float_op_bits(const StrSet *fs, const IROperand *a,
                         const IROperand *b, int instruction_bits) {
  if (instruction_bits == 32 || instruction_bits == 64) return instruction_bits;
  const IROperand *ops[2] = {a, b};
  int known = 0, all_single = 1;
  for (int i = 0; i < 2; i++) {
    if (!ops[i] || ops[i]->kind == IR_OPERAND_NONE) continue;
    if (ops[i]->kind == IR_OPERAND_FLOAT) continue;
    known = 1;
    if (operand_float_bits(fs, ops[i]) != 32) all_single = 0;
  }
  return known && all_single ? 32 : 64;
}

static int prog_fn_index(const IRProgram *prog, const char *name) {
  if (!prog || !name) return -1;
  for (size_t i = 0; i < prog->function_count; i++) {
    if (strcmp(prog->functions[i]->name, name) == 0) return (int)i;
  }
  return -1;
}

static int cast_target_float_bits(const IRInstruction *in) {
  if (in->dest.float_bits != 0) {
    return in->dest.float_bits;
  }
  return type_text_is_float_scalar(in->text) ? type_text_float_bits(in->text) : 0;
}

static void float_set_note_call(const IRInstruction *in, const IRProgram *prog,
                                const int *retf, StrSet *fs) {
  if (in->op == IR_OP_CALL_INDIRECT) {
    if (in->value_type && mtlc_type_is_float(in->value_type)) {
      set_add_bits(fs, in->dest.name,
                   in->value_type->kind == MTLC_TYPE_FLOAT32 ? 32 : 64);
    }
    return;
  }
  const IRModuleSymbol *callee = module_function(prog, in->text);
  int ci = retf ? prog_fn_index(prog, in->text) : -1;
  if ((callee && mtlc_type_is_float(callee->return_type)) ||
      (ci >= 0 && retf[ci])) {
    set_add_bits(fs, in->dest.name,
                 callee && callee->return_type
                     ? (callee->return_type->kind == MTLC_TYPE_FLOAT32 ? 32 : 64)
                     : 0);
  }
}

static void float_set_note_result(const IRInstruction *in, const IRProgram *prog,
                                  const int *retf, StrSet *fs) {
  switch (in->op) {
  case IR_OP_LOAD:
    if (in->is_float) {
      int loaded = in->rhs.kind == IR_OPERAND_INT ? (int)in->rhs.int_value : 8;
      set_add_bits(fs, in->dest.name,
                   in->float_bits ? in->float_bits : (loaded == 4 ? 32 : 64));
    }
    break;
  case IR_OP_BINARY:
  case IR_OP_UNARY:
    if (in->is_float) {
      set_add_bits(fs, in->dest.name,
                   in->float_bits ? in->float_bits
                                  : float_op_bits(fs, &in->lhs, &in->rhs, 0));
    }
    break;
  case IR_OP_CAST:
    if (cast_target_float_bits(in) != 0)
      set_add_bits(fs, in->dest.name, cast_target_float_bits(in));
    break;
  case IR_OP_ASSIGN:
    if (operand_is_float(fs, &in->lhs))
      set_add_bits(fs, in->dest.name, operand_float_bits(fs, &in->lhs));
    break;
  case IR_OP_CALL_INDIRECT:
  case IR_OP_CALL:
    float_set_note_call(in, prog, retf, fs);
    break;
  default:
    break;
  }
}

static void build_float_set(const IRFunction *fn, const IRProgram *prog,
                            const int *retf, StrSet *fs) {
  for (size_t i = 0; prog && i < prog->module_symbol_count; i++) {
    const IRModuleSymbol *symbol = &prog->module_symbols[i];
    if (symbol->kind == IR_MODSYM_VARIABLE && mtlc_type_is_float(symbol->type)) {
      set_add_bits(fs, symbol->name,
                   symbol->type->kind == MTLC_TYPE_FLOAT32 ? 32 : 64);
    }
  }
  for (size_t i = 0; i < fn->parameter_count; i++) {
    if (fn->parameter_types &&
        type_text_is_float_scalar(fn->parameter_types[i])) {
      set_add_bits(fs, fn->parameter_names[i],
                   type_text_float_bits(fn->parameter_types[i]));
    }
  }
  for (size_t i = 0; i < fn->instruction_count; i++) {
    const IRInstruction *in = &fn->instructions[i];
    if (in->op == IR_OP_DECLARE_LOCAL && in->dest.name &&
        type_text_is_float_scalar(in->text)) {
      set_add_bits(fs, in->dest.name, type_text_float_bits(in->text));
    }
  }
  for (int pass = 0; pass < 4; pass++) {
    for (size_t i = 0; i < fn->instruction_count; i++) {
      const IRInstruction *in = &fn->instructions[i];
      if (in->dest.kind == IR_OPERAND_TEMP || in->dest.kind == IR_OPERAND_SYMBOL) {
        float_set_note_result(in, prog, retf, fs);
      }
    }
  }
}

static int cmp_cond(const char *op);

static int type_is_unsigned(const MtlcType *t) {
  if (!t) return 0;
  switch (t->kind) {
  case MTLC_TYPE_UINT8:
  case MTLC_TYPE_UINT16:
  case MTLC_TYPE_UINT32:
  case MTLC_TYPE_UINT64:
  case MTLC_TYPE_POINTER:
  case MTLC_TYPE_FUNCTION_POINTER:
  case MTLC_TYPE_STRING:
    return 1;
  default:
    return 0;
  }
}

static int type_text_is_unsigned(const char *t) {
  if (!t) return 0;
  if (strchr(t, '*') != NULL) return 1;
  return type_text_base_is(t, "uint8") || type_text_base_is(t, "uint16") ||
         type_text_base_is(t, "uint32") || type_text_base_is(t, "uint64");
}

static void build_unsigned_set(const IRFunction *fn, StrSet *us) {
  for (size_t i = 0; i < fn->parameter_count; i++) {
    if (fn->parameter_types && type_text_is_unsigned(fn->parameter_types[i])) {
      set_add(us, fn->parameter_names[i]);
    }
  }
  for (size_t i = 0; i < fn->instruction_count; i++) {
    const IRInstruction *in = &fn->instructions[i];
    if (in->op == IR_OP_DECLARE_LOCAL && in->dest.name &&
        (type_text_is_unsigned(in->text) || type_is_unsigned(in->value_type))) {
      set_add(us, in->dest.name);
    }
  }
  for (int pass = 0; pass < 4; pass++) {
    for (size_t i = 0; i < fn->instruction_count; i++) {
      const IRInstruction *in = &fn->instructions[i];
      if (in->dest.kind != IR_OPERAND_TEMP && in->dest.kind != IR_OPERAND_SYMBOL)
        continue;
      switch (in->op) {
      case IR_OP_LOAD:
      case IR_OP_UNARY:
        if (in->is_unsigned) set_add(us, in->dest.name);
        break;
      case IR_OP_BINARY:
        if ((in->is_unsigned || type_is_unsigned(in->value_type)) &&
            in->text && cmp_cond(in->text) < 0) {
          set_add(us, in->dest.name);
        }
        break;
      case IR_OP_ADDRESS_OF:
        set_add(us, in->dest.name);
        break;
      case IR_OP_CAST:
        if (type_is_unsigned(in->value_type)) set_add(us, in->dest.name);
        break;
      case IR_OP_ASSIGN:
        if ((in->lhs.kind == IR_OPERAND_TEMP ||
             in->lhs.kind == IR_OPERAND_SYMBOL) &&
            set_has(us, in->lhs.name)) {
          set_add(us, in->dest.name);
        }
        break;
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
  free(fs.bits);
  return rf;
}

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
      arm64_fail(e, "could not place a string literal in .rodata");
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

/* A call names an extern by its Mettle name; the stub table is keyed by the
   link name the extern gives (`memory_region = "mettle_safety_region_begin"`),
   so look the stub up under both. */
static int call_stub_index(const IRProgram *prog, const char *name) {
  int stub = runtime_stub_index(name);
  if (stub >= 0) return stub;
  const IRModuleSymbol *callee = module_function(prog, name);
  const char *link_name = callee ? module_link_name(callee) : NULL;
  return link_name ? runtime_stub_index(link_name) : -1;
}

static int emit_external_call(Arm64Emit *e, Arm64ObjectContext *object,
                              const IRProgram *prog, const char *name) {
  const IRModuleSymbol *callee = module_function(prog, name);
  const char *link_name = callee ? module_link_name(callee) : name;
  if (!binary_emitter_find_symbol(object->emitter, link_name) &&
      !binary_emitter_declare_external(object->emitter, link_name)) {
    arm64_fail(e, "could not declare external symbol '%s'", link_name);
    return 0;
  }
  size_t call_offset = arm64_here(e);
  arm64_emit_word(e, arm64_bl(0));
  return object_add_relocation(e, object, call_offset,
                               BINARY_RELOCATION_ARM64_CALL26, link_name);
}

static int emit_runtime_call(Arm64Emit *e, Arm64ObjectContext *object,
                             const IRProgram *prog, LblMap *fns, int stub) {
  if (object) {
    return emit_external_call(e, object, prog, RUNTIME_STUBS[stub].name);
  }
  arm64_emit_bl(e, label_for(e, fns, RUNTIME_STUBS[stub].name));
  return 1;
}

static void emit_function_address(Arm64Emit *e, SlotMap *s, Arm64Reg rd,
                                  const char *name) {
  if (s->object) {
    const IRModuleSymbol *symbol = module_function(s->program, name);
    emit_symbol_address(e, s->object, rd,
                        symbol ? module_link_name(symbol) : name);
    return;
  }
  arm64_emit_label_address(e, rd, label_for(e, s->fns, name));
}

static void emit_string_value(Arm64Emit *e, SlotMap *s, Arm64Reg dest,
                              const char *str, size_t length) {
  if (!str) {
    str = "";
    length = 0;
  }
  if (s->object) {
    Arm64ObjectContext *object = s->object;
    char chars_symbol[64], record_symbol[64];
    size_t chars_offset = 0, record_offset = 0;
    uint64_t encoded_length = (uint64_t)length;
    BinarySection *rodata = NULL;
    snprintf(chars_symbol, sizeof(chars_symbol), ".Lmtlc.str.%u",
             object->string_id++);
    snprintf(record_symbol, sizeof(record_symbol), ".Lmtlc.str.%u",
             object->string_id++);
    if (!binary_emitter_append_bytes(object->emitter, object->rodata_section,
                                     str, length + 1, &chars_offset) ||
        !binary_emitter_define_symbol(object->emitter, chars_symbol,
                                      BINARY_SYMBOL_LOCAL,
                                      object->rodata_section, chars_offset,
                                      length + 1) ||
        !binary_emitter_align_section(object->emitter, object->rodata_section, 8,
                                      0) ||
        !binary_emitter_append_zeros(object->emitter, object->rodata_section, 16,
                                     &record_offset) ||
        !binary_emitter_define_symbol(object->emitter, record_symbol,
                                      BINARY_SYMBOL_LOCAL,
                                      object->rodata_section, record_offset,
                                      16) ||
        !binary_emitter_add_relocation(object->emitter, object->rodata_section,
                                       record_offset, BINARY_RELOCATION_ADDR64,
                                       chars_symbol, 0)) {
      arm64_fail(e, "could not place a string literal in .rodata");
      return;
    }
    rodata = binary_emitter_get_section(object->emitter, object->rodata_section);
    if (!rodata || !rodata->data || record_offset + 16 > rodata->size) {
      arm64_fail(e, "could not write a string literal's length");
      return;
    }
    memcpy(rodata->data + record_offset + 8, &encoded_length, 8);
    emit_symbol_address(e, object, dest, record_symbol);
    return;
  }
  if (!s->data) {
    arm64_fail(e, "no data image to hold a string literal");
    return;
  }
  size_t chars = data_reserve(s->data, length + 1, 1);
  if (chars == (size_t)-1) {
    arm64_fail(e, "out of memory storing a string literal");
    return;
  }
  memcpy(s->data->bytes + chars, str, length + 1);
  size_t record = data_reserve(s->data, 16, 8);
  if (record == (size_t)-1) {
    arm64_fail(e, "out of memory storing a string literal's record");
    return;
  }
  uint64_t address = (uint64_t)ELF_DATA_VADDR + (uint64_t)chars;
  uint64_t encoded_length = (uint64_t)length;
  memcpy(s->data->bytes + record, &address, 8);
  memcpy(s->data->bytes + record + 8, &encoded_length, 8);
  emit_imm(e, dest, (uint64_t)ELF_DATA_VADDR + (uint64_t)record);
}

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
    emit_string_value(e, s, dest, op->name, ir_operand_string_length(op));
    return dest;
  case IR_OPERAND_TEMP:
  case IR_OPERAND_SYMBOL: {
    int off = slot_find(s, op->name);
    const IRModuleSymbol *global =
        off < 0 && op->kind == IR_OPERAND_SYMBOL ? module_variable(s, op->name)
                                                 : NULL;
    if (global) {
      if (type_is_aggregate(global->type)) {
        emit_global_address(e, s, dest, global);
        return dest;
      }
      emit_global_address(e, s, ARM64_X16, global);
      emit_load_sized(e, dest, ARM64_X16, module_type_size(global->type),
                      module_type_signed(global->type));
      return dest;
    }
    if (off < 0 && op->kind == IR_OPERAND_SYMBOL &&
        prog_fn_index(s->program, op->name) >= 0) {
      emit_function_address(e, s, dest, op->name);
      return dest;
    }
    if (off < 0) off = slot_off(s, op->name);
    if (off < 0) {
      arm64_fail(e, "no stack slot for '%s'", op->name ? op->name : "(unnamed)");
      return dest;
    }
    if (op->kind == IR_OPERAND_SYMBOL &&
        agg_size_of(s->aggregates, op->name) > 8) {
      emit_lea_local(e, dest, off);
      return dest;
    }
    int is_signed = 0;
    int width = narrow_size_of(s->narrow, op->name, &is_signed);
    if (width) {
      Arm64Reg base = dest == ARM64_X17 ? ARM64_X16 : ARM64_X17;
      emit_lea_local(e, base, off);
      emit_load_sized(e, dest, base, width, is_signed);
      return dest;
    }
    emit_slot_ldr(e, dest, off);
    return dest;
  }
  default:
    arm64_fail(e, "operand kind %d cannot be loaded", (int)op->kind);
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
    emit_global_address(e, s, ARM64_X16, global);
    emit_store_sized(e, src, ARM64_X16, module_type_size(global->type));
    return;
  }
  if (off < 0) off = slot_off(s, dst->name);
  if (off < 0) {
    arm64_fail(e, "no stack slot for destination '%s'",
               dst && dst->name ? dst->name : "(unnamed)");
    return;
  }
  int width = narrow_size_of(s->narrow, dst->name, NULL);
  if (width) {
    Arm64Reg base = src == ARM64_X17 ? ARM64_X16 : ARM64_X17;
    emit_lea_local(e, base, off);
    emit_store_sized(e, src, base, width);
    return;
  }
  emit_slot_str(e, src, off);
}

static void emit_lea_local(Arm64Emit *e, Arm64Reg rd, int off) {
  if (off <= 4095) {
    arm64_emit_word(e, arm64_add_imm(1, rd, ARM64_SP, (uint32_t)off, 0));
    return;
  }
  if (off > 0xFFFFFF) {
    arm64_fail(e, "frame offset %d exceeds the 16MB lea reach", off);
    return;
  }
  arm64_emit_word(e, arm64_add_imm(1, rd, ARM64_SP, (uint32_t)off & 0xFFF, 0));
  arm64_emit_word(e, arm64_add_imm(1, rd, rd, (uint32_t)off >> 12, 1));
}

static void emit_slot_ldr(Arm64Emit *e, Arm64Reg rt, int off) {
  if (off >= 0 && off <= 32760) {
    arm64_emit_word(e, arm64_ldr_imm(1, rt, ARM64_SP, off));
    return;
  }
  Arm64Reg base = rt == ARM64_X16 ? ARM64_X17 : ARM64_X16;
  emit_lea_local(e, base, off);
  arm64_emit_word(e, arm64_ldr_imm(1, rt, base, 0));
}

static void emit_slot_str(Arm64Emit *e, Arm64Reg rt, int off) {
  if (off >= 0 && off <= 32760) {
    arm64_emit_word(e, arm64_str_imm(1, rt, ARM64_SP, off));
    return;
  }
  Arm64Reg base = rt == ARM64_X16 ? ARM64_X17 : ARM64_X16;
  emit_lea_local(e, base, off);
  arm64_emit_word(e, arm64_str_imm(1, rt, base, 0));
}

static void emit_slot_str_fp(Arm64Emit *e, int is_double, int ft, int off) {
  int limit = is_double ? 32760 : 16380;
  if (off >= 0 && off <= limit) {
    arm64_emit_word(e, arm64_str_fp(is_double, ft, ARM64_SP, off));
    return;
  }
  emit_lea_local(e, ARM64_X16, off);
  arm64_emit_word(e, arm64_str_fp(is_double, ft, ARM64_X16, 0));
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

static int cmp_cond_unsigned(const char *op) {
  if (strcmp(op, "<") == 0) return ARM64_CC;
  if (strcmp(op, "<=") == 0) return ARM64_LS;
  if (strcmp(op, ">") == 0) return ARM64_HI;
  if (strcmp(op, ">=") == 0) return ARM64_CS;
  return cmp_cond(op);
}

static int cmp_cond_float(const char *op) {
  if (strcmp(op, "<") == 0) return ARM64_MI;
  if (strcmp(op, "<=") == 0) return ARM64_LS;
  return cmp_cond(op);
}

static int binary_is_unsigned(const StrSet *us, const IRInstruction *in) {
  if (in->is_unsigned || type_is_unsigned(in->value_type)) return 1;
  const IROperand *ops[2] = {&in->lhs, &in->rhs};
  for (int k = 0; k < 2; k++) {
    if ((ops[k]->kind == IR_OPERAND_TEMP ||
         ops[k]->kind == IR_OPERAND_SYMBOL) &&
        set_has(us, ops[k]->name)) {
      return 1;
    }
  }
  return 0;
}

static void load_float_operand(Arm64Emit *e, SlotMap *s, const StrSet *fs,
                               const IROperand *op, int d, int fd,
                               Arm64Reg scratch) {
  if (op->kind == IR_OPERAND_FLOAT) {
    emit_imm(e, scratch, ieee_bits_at(op, d ? 64 : 32));
    arm64_emit_word(e, arm64_fmov_gp(d, fd, scratch));
    return;
  }
  Arm64Reg bits = load_into(e, s, op, scratch);
  if (operand_is_float(fs, op)) {
    int src_d = operand_float_bits(fs, op) != 32;
    arm64_emit_word(e, arm64_fmov_gp(src_d, fd, bits));
    if (src_d != d) {
      arm64_emit_word(e, arm64_fcvt(d, fd, fd));
    }
  } else {
    arm64_emit_word(e, arm64_scvtf(d, fd, bits));
  }
}

static void lower_binary(Arm64Emit *e, SlotMap *s, const StrSet *fs,
                         const StrSet *us, const IRInstruction *in) {
  const char *op = in->text;

  if (in->is_float) {
    int fcc = cmp_cond(op) < 0 ? -1 : cmp_cond_float(op);
    int d = float_op_bits(fs, &in->lhs, &in->rhs, in->float_bits) != 32;
    load_float_operand(e, s, fs, &in->lhs, d, 0, R_LHS);
    load_float_operand(e, s, fs, &in->rhs, d, 1, R_RHS);
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
      arm64_fail(e, "float binary operator '%s' has no AArch64 lowering", op);
      return;
    }
    if (fcc < 0) {
      arm64_emit_word(e, arm64_fmov_to_gp(d, R_RES, 0));
    }
    store_dest(e, s, &in->dest, R_RES);
    return;
  }

  int uns = binary_is_unsigned(us, in);
  Arm64Reg a = load_into(e, s, &in->lhs, R_LHS);

  if (in->rhs.kind == IR_OPERAND_INT && op[0] != '\0' && op[1] == '\0') {
    CgStrengthRewrite rw;
    if (cg_strength_classify(op[0], in->rhs.int_value, uns, &rw)) {
      switch (rw.kind) {
      case CG_SR_MUL_SHL:
        arm64_emit_word(e, arm64_lsl_imm(1, R_RES, a, rw.shift));
        store_dest(e, s, &in->dest, R_RES);
        return;
      case CG_SR_MUL_SHL_ADD:
        arm64_emit_word(e, arm64_lsl_imm(1, R_AUX, a, rw.shift));
        arm64_emit_word(e, arm64_add_reg(1, R_RES, R_AUX, a));
        store_dest(e, s, &in->dest, R_RES);
        return;
      case CG_SR_MUL_SHL_SUB:
        arm64_emit_word(e, arm64_lsl_imm(1, R_AUX, a, rw.shift));
        arm64_emit_word(e, arm64_sub_reg(1, R_RES, R_AUX, a));
        store_dest(e, s, &in->dest, R_RES);
        return;
      case CG_SR_UDIV_SHR:
        arm64_emit_word(e, arm64_lsr_imm(1, R_RES, a, rw.shift));
        store_dest(e, s, &in->dest, R_RES);
        return;
      case CG_SR_UREM_AND:
        emit_imm(e, R_AUX, (uint64_t)rw.mask);
        arm64_emit_word(e, arm64_and_reg(1, R_RES, a, R_AUX));
        store_dest(e, s, &in->dest, R_RES);
        return;
      default:
        break;
      }
    }
  }

  Arm64Reg b = load_into(e, s, &in->rhs, R_RHS);
  int cc = cmp_cond(op) < 0 ? -1 : (uns ? cmp_cond_unsigned(op) : cmp_cond(op));
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
    arm64_emit_word(e, uns ? arm64_udiv(1, R_RES, a, b)
                           : arm64_sdiv(1, R_RES, a, b));
  } else if (strcmp(op, "%") == 0) {
    arm64_emit_word(e, uns ? arm64_udiv(1, R_AUX, a, b)
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
    arm64_emit_word(e, uns ? arm64_lsrv(1, R_RES, a, b)
                           : arm64_asrv(1, R_RES, a, b));
  } else {
    arm64_fail(e, "binary operator '%s' has no AArch64 lowering", op);
    return;
  }
  store_dest(e, s, &in->dest, R_RES);
}

static void lower_unary(Arm64Emit *e, SlotMap *s, const StrSet *fs,
                        const IRInstruction *in) {
  const char *op = in->text;
  if (in->is_float || operand_is_float(fs, &in->lhs)) {
    int d = float_op_bits(fs, &in->lhs, NULL, in->float_bits) != 32;
    if (strcmp(op, "-") == 0) {
      load_float_operand(e, s, fs, &in->lhs, d, 0, R_LHS);
      arm64_emit_word(e, arm64_fneg(d, 0, 0));
      arm64_emit_word(e, arm64_fmov_to_gp(d, R_RES, 0));
      store_dest(e, s, &in->dest, R_RES);
      return;
    }
    if (strcmp(op, "!") == 0) {
      load_float_operand(e, s, fs, &in->lhs, d, 0, R_LHS);
      emit_imm(e, R_LHS, 0);
      arm64_emit_word(e, arm64_fmov_gp(d, 1, R_LHS));
      arm64_emit_word(e, arm64_fcmp(d, 0, 1));
      arm64_emit_word(e, arm64_cset(1, R_RES, ARM64_EQ));
      store_dest(e, s, &in->dest, R_RES);
      return;
    }
    arm64_fail(e, "float unary operator '%s' has no AArch64 lowering", op);
    return;
  }
  Arm64Reg a = load_into(e, s, &in->lhs, R_LHS);
  if (strcmp(op, "-") == 0) {
    arm64_emit_word(e, arm64_neg(1, R_RES, a));
  } else if (strcmp(op, "~") == 0) {
    arm64_emit_word(e, arm64_mvn(1, R_RES, a));
  } else if (strcmp(op, "!") == 0) {
    arm64_emit_word(e, arm64_cmp_imm(1, a, 0, 0));
    arm64_emit_word(e, arm64_cset(1, R_RES, ARM64_EQ));
  } else {
    arm64_fail(e, "unary operator '%s' has no AArch64 lowering", op);
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

static const MtlcType *call_arg_type(const IRProgram *program,
                                     const IRInstruction *call, size_t index) {
  const IRModuleSymbol *callee = module_function(program, call->text);
  if (callee && index < callee->param_count && callee->param_types &&
      callee->param_types[index]) {
    return callee->param_types[index];
  }
  if (call->argument_types && index < call->argument_count &&
      call->argument_types[index]) {
    return call->argument_types[index];
  }
  return NULL;
}

static int call_arg_is_float(const IRProgram *program,
                             const IRInstruction *call, size_t index,
                             const StrSet *floats) {
  const MtlcType *type = call_arg_type(program, call, index);
  if (type) {
    return mtlc_type_is_float(type);
  }
  return operand_is_float(floats, &call->arguments[index]);
}

static int call_arg_float_bits(const IRProgram *program,
                               const IRInstruction *call, size_t index) {
  const MtlcType *type = call_arg_type(program, call, index);
  if (type) {
    return type->kind == MTLC_TYPE_FLOAT32 ? 32 : 64;
  }
  return call->arguments[index].float_bits == 32 ? 32 : 64;
}

static int call_returns_float(const IRProgram *program,
                              const IRInstruction *call, const int *retf) {
  if (call->op == IR_OP_CALL_INDIRECT) {
    return call->value_type && mtlc_type_is_float(call->value_type);
  }
  const IRModuleSymbol *callee = module_function(program, call->text);
  if (callee && callee->return_type) {
    return mtlc_type_is_float(callee->return_type);
  }
  int index = retf ? prog_fn_index(program, call->text) : -1;
  return index >= 0 && retf[index];
}

static int call_return_float_bits(const IRProgram *program,
                                  const IRInstruction *call) {
  if (call->op == IR_OP_CALL_INDIRECT && call->value_type) {
    return call->value_type->kind == MTLC_TYPE_FLOAT32 ? 32 : 64;
  }
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
    int indirect = call->op == IR_OP_CALL_INDIRECT;
    if ((call->op != IR_OP_CALL && !indirect) || call->argument_count == 0 ||
        (!indirect && (!call->text || strcmp(call->text, "cstr") == 0))) {
      continue;
    }
    if (call->argument_count > (size_t)INT32_MAX) {
      arm64_fail(e, "call to '%s' has an implausible argument count",
                 indirect ? "(indirect)" : call->text);
      return 0;
    }
    int count = (int)call->argument_count;
    int *is_float = malloc((size_t)count * sizeof(*is_float));
    Arm64ArgLocation *locations =
        malloc((size_t)count * sizeof(*locations));
    if (!is_float || !locations) {
      free(is_float);
      free(locations);
      arm64_fail(e, "out of memory laying out the arguments of '%s'",
                 indirect ? "(indirect)" : call->text);
      return 0;
    }
    int *stack_sizes = calloc((size_t)count, sizeof(*stack_sizes));
    if (!stack_sizes) {
      free(is_float);
      free(locations);
      arm64_fail(e, "out of memory laying out the arguments of '%s'",
                 indirect ? "(indirect)" : call->text);
      return 0;
    }
    for (int k = 0; k < count; k++) {
      is_float[k] = call_arg_is_float(program, call, (size_t)k, floats);
      stack_sizes[k] =
          stack_size_of_type(call_arg_type(program, call, (size_t)k));
    }
    int bytes = 0;
    if (!arm64_compute_arg_layout_packed(is_float, stack_sizes, count,
                                         locations, &bytes)) {
      arm64_fail(e, "cannot lay out the %d arguments of '%s' under AAPCS64",
                 count, indirect ? "(indirect)" : call->text);
    }
    if (bytes > maximum) maximum = bytes;
    free(stack_sizes);
    free(is_float);
    free(locations);
    if (e->error) return 0;
  }
  return (maximum + 15) & ~15;
}

static int operand_needs_slot(const SlotMap *slots, const IRProgram *prog,
                              const IROperand *op) {
  if (op->kind == IR_OPERAND_TEMP) return 1;
  if (op->kind != IR_OPERAND_SYMBOL || !op->name) return 0;
  if (module_variable(slots, op->name)) return 0;
  return prog_fn_index(prog, op->name) < 0;
}

static void build_narrow_map(const IRFunction *fn, const IRProgram *prog,
                             NarrowMap *nm) {
  const IRModuleSymbol *self = module_function(prog, fn->name);
  for (size_t i = 0; i < fn->parameter_count; i++) {
    if (!self || i >= self->param_count || !self->param_types) break;
    const MtlcType *t = self->param_types[i];
    if (!t || mtlc_type_is_float(t) || type_is_aggregate(t)) continue;
    size_t size = mtlc_type_size(t);
    if (size == 1 || size == 2 || size == 4) {
      narrow_add(nm, fn->parameter_names[i], (int)size, module_type_signed(t));
    }
  }
  for (size_t i = 0; i < fn->instruction_count; i++) {
    const IRInstruction *in = &fn->instructions[i];
    if (in->op != IR_OP_DECLARE_LOCAL || !in->dest.name) continue;
    const MtlcType *t = declared_type(prog, in);
    if (!t || mtlc_type_is_float(t) || type_is_aggregate(t)) continue;
    size_t size = mtlc_type_size(t);
    if (size == 1 || size == 2 || size == 4) {
      narrow_add(nm, in->dest.name, (int)size, module_type_signed(t));
    }
  }
}

static void build_aggregate_set(const IRFunction *fn, const IRProgram *prog,
                                AggMap *ag) {
  for (size_t i = 0; i < fn->instruction_count; i++) {
    const IRInstruction *in = &fn->instructions[i];
    if (in->op == IR_OP_DECLARE_LOCAL && in->dest.name &&
        type_is_aggregate(declared_type(prog, in))) {
      agg_add(ag, in->dest.name, declare_local_size(prog, in));
    }
  }
}

static Arm64Reg address_operand(Arm64Emit *e, SlotMap *s, const AggMap *ag,
                                const IROperand *op, Arm64Reg dest) {
  if (op->kind == IR_OPERAND_SYMBOL && agg_size_of(ag, op->name) > 0) {
    int off = slot_find(s, op->name);
    if (off >= 0) {
      emit_lea_local(e, dest, off);
      return dest;
    }
  }
  return load_into(e, s, op, dest);
}

#define ARM64_COPY_UNROLL_MAX 128

static void emit_fixed_copy(Arm64Emit *e, Arm64Reg dst, Arm64Reg src,
                            int size) {
  int off = 0;
  if (size > ARM64_COPY_UNROLL_MAX) {
    int words = size / 8;
    int loop = arm64_new_label(e);
    arm64_emit_word(e, arm64_mov_reg(1, ARM64_X13, dst));
    arm64_emit_word(e, arm64_mov_reg(1, ARM64_X14, src));
    emit_imm(e, ARM64_X15, (uint64_t)words);
    arm64_bind_label(e, loop);
    arm64_emit_word(e, arm64_ldr_imm(1, R_AUX, ARM64_X14, 0));
    arm64_emit_word(e, arm64_str_imm(1, R_AUX, ARM64_X13, 0));
    arm64_emit_word(e, arm64_add_imm(1, ARM64_X14, ARM64_X14, 8, 0));
    arm64_emit_word(e, arm64_add_imm(1, ARM64_X13, ARM64_X13, 8, 0));
    arm64_emit_word(e, arm64_sub_imm(1, ARM64_X15, ARM64_X15, 1, 0));
    arm64_emit_cbnz(e, 1, ARM64_X15, loop);
    dst = ARM64_X13;
    src = ARM64_X14;
    size -= words * 8;
  }
  while (size - off >= 8) {
    arm64_emit_word(e, arm64_ldr_imm(1, R_AUX, src, off));
    arm64_emit_word(e, arm64_str_imm(1, R_AUX, dst, off));
    off += 8;
  }
  if (size - off >= 4) {
    arm64_emit_word(e, arm64_ldr_imm(0, R_AUX, src, off));
    arm64_emit_word(e, arm64_str_imm(0, R_AUX, dst, off));
    off += 4;
  }
  if (size - off >= 2) {
    arm64_emit_word(e, arm64_ldrh_imm(R_AUX, src, off));
    arm64_emit_word(e, arm64_strh_imm(R_AUX, dst, off));
    off += 2;
  }
  if (size - off >= 1) {
    arm64_emit_word(e, arm64_ldrb_imm(R_AUX, src, off));
    arm64_emit_word(e, arm64_strb_imm(R_AUX, dst, off));
  }
}

static int call_uses_io_stub(const SlotMap *s, const IRInstruction *call,
                             int *is_string) {
  return !s->object && call->text &&
         io_stub_intrinsic(call->text, NULL, is_string);
}

static int arg_is_wide_aggregate(const IRProgram *prog, SlotMap *s,
                                 const IRInstruction *call,
                                 const IROperand *arg) {
  if (arg->kind != IR_OPERAND_SYMBOL) return 0;
  if (call_uses_io_stub(s, call, NULL)) return 0;
  if (agg_size_of(s->aggregates, arg->name) > 8) return 1;
  const IRModuleSymbol *global = module_variable(s, arg->name);
  (void)prog;
  return global && type_is_aggregate(global->type) &&
         mtlc_type_size(global->type) > 8;
}

static void aggregate_address(Arm64Emit *e, SlotMap *s, const IROperand *op,
                              Arm64Reg dest) {
  const IRModuleSymbol *global =
      slot_find(s, op->name) < 0 ? module_variable(s, op->name) : NULL;
  if (global) {
    emit_global_address(e, s, dest, global);
  } else {
    emit_lea_local(e, dest, slot_off(s, op->name));
  }
}

static int fn_aggregate_return_size(const IRProgram *prog, const char *name) {
  const IRModuleSymbol *symbol = module_function(prog, name);
  if (!symbol || !symbol->return_type ||
      !type_is_aggregate(symbol->return_type)) {
    return 0;
  }
  size_t size = mtlc_type_size(symbol->return_type);
  return size > 8 && size <= INT_MAX ? (int)size : 0;
}

static const char SRET_SAVE_SLOT[] = "..sret.save";
static const char SRET_BUFFER_SLOT[] = "..sret.buf";
static const char CONCAT_A_SLOT[] = "..concat.a";
static const char CONCAT_B_SLOT[] = "..concat.b";
static const char CONCAT_DST_SLOT[] = "..concat.dst";

static int binary_is_string_concat(const IRInstruction *in) {
  return in->op == IR_OP_CALL && in->text && in->argument_count == 2 &&
         strcmp(in->text, "mettle_string_concat") == 0;
}

static void emit_concat_operand(Arm64Emit *e, SlotMap *slots, AggMap *ag,
                                const IROperand *operand, int slot) {
  if (operand->kind == IR_OPERAND_SYMBOL &&
      agg_size_of(ag, operand->name) > 0) {
    emit_lea_local(e, R_LHS, slot_off(slots, operand->name));
  } else {
    load_into(e, slots, operand, R_LHS);
  }
  emit_slot_str(e, R_LHS, slot);
}

static void emit_concat_lengths(Arm64Emit *e, int off_a, int off_b,
                                Arm64Reg total) {
  emit_slot_ldr(e, R_LHS, off_a);
  arm64_emit_word(e, arm64_ldr_imm(1, R_LHS, R_LHS, 8));
  emit_slot_ldr(e, R_RHS, off_b);
  arm64_emit_word(e, arm64_ldr_imm(1, R_RHS, R_RHS, 8));
  arm64_emit_word(e, arm64_add_reg(1, total, R_LHS, R_RHS));
}

static void emit_concat_header_offset(Arm64Emit *e, Arm64Reg total,
                                      Arm64Reg out) {
  arm64_emit_word(e, arm64_add_imm(1, out, total, 8, 0));
  arm64_emit_word(e, arm64_lsr_imm(1, out, out, 3));
  arm64_emit_word(e, arm64_lsl_imm(1, out, out, 3));
}

static void emit_concat_copy(Arm64Emit *e, Arm64ObjectContext *object,
                             const IRProgram *prog, LblMap *fns, int off_dst,
                             int off_piece, int after_a, int off_a) {
  emit_slot_ldr(e, ARM64_X0, off_dst);
  if (after_a) {
    emit_slot_ldr(e, R_LHS, off_a);
    arm64_emit_word(e, arm64_ldr_imm(1, R_LHS, R_LHS, 8));
    arm64_emit_word(e, arm64_add_reg(1, ARM64_X0, ARM64_X0, R_LHS));
  }
  emit_slot_ldr(e, R_LHS, off_piece);
  arm64_emit_word(e, arm64_ldr_imm(1, ARM64_X1, R_LHS, 0));
  arm64_emit_word(e, arm64_ldr_imm(1, ARM64_X2, R_LHS, 8));
  emit_runtime_call(e, object, prog, fns, STUB_MEMCPY);
}

static void emit_string_concat(Arm64Emit *e, SlotMap *slots, AggMap *ag,
                               Arm64ObjectContext *object,
                               const IRProgram *prog, LblMap *fns,
                               const IRInstruction *in) {
  int off_a = slot_off(slots, CONCAT_A_SLOT);
  int off_b = slot_off(slots, CONCAT_B_SLOT);
  int off_dst = slot_off(slots, CONCAT_DST_SLOT);
  emit_concat_operand(e, slots, ag, &in->arguments[0], off_a);
  emit_concat_operand(e, slots, ag, &in->arguments[1], off_b);
  emit_concat_lengths(e, off_a, off_b, ARM64_X0);
  emit_concat_header_offset(e, ARM64_X0, ARM64_X0);
  arm64_emit_word(e, arm64_add_imm(1, ARM64_X0, ARM64_X0, 16, 0));
  emit_runtime_call(e, object, prog, fns, STUB_MALLOC);
  emit_slot_str(e, ARM64_X0, off_dst);
  emit_concat_copy(e, object, prog, fns, off_dst, off_a, 0, off_a);
  emit_concat_copy(e, object, prog, fns, off_dst, off_b, 1, off_a);
  emit_concat_lengths(e, off_a, off_b, ARM64_X1);
  emit_slot_ldr(e, ARM64_X0, off_dst);
  arm64_emit_word(e, arm64_add_reg(1, ARM64_X2, ARM64_X0, ARM64_X1));
  arm64_emit_word(e, arm64_strb_imm(ARM64_XZR, ARM64_X2, 0));
  emit_concat_header_offset(e, ARM64_X1, ARM64_X2);
  arm64_emit_word(e, arm64_add_reg(1, R_RES, ARM64_X0, ARM64_X2));
  arm64_emit_word(e, arm64_str_imm(1, ARM64_X0, R_RES, 0));
  arm64_emit_word(e, arm64_str_imm(1, ARM64_X1, R_RES, 8));
  if (in->dest.kind == IR_OPERAND_SYMBOL &&
      agg_size_of(ag, in->dest.name) > 8) {
    emit_lea_local(e, R_RHS, slot_off(slots, in->dest.name));
    emit_fixed_copy(e, R_RHS, R_RES, 16);
  } else {
    store_dest(e, slots, &in->dest, R_RES);
  }
}

static int type_is_cstring(const MtlcType *type) {
  if (!type || type->kind != MTLC_TYPE_POINTER) return 0;
  if (type->name && strcmp(type->name, "cstring") == 0) return 1;
  return type->base_type && type->base_type->name &&
         strcmp(type->base_type->name, "uint8") == 0;
}

static void load_call_argument(Arm64Emit *e, SlotMap *slots,
                               const IRProgram *prog,
                               const IRInstruction *call, const IROperand *arg,
                               Arm64Reg dest) {
  int stub_is_string = 0;
  int is_stub = call_uses_io_stub(slots, call, &stub_is_string);
  if (is_stub &&
      (arg->kind == IR_OPERAND_STRING ||
       (stub_is_string && arg->kind == IR_OPERAND_TEMP) ||
       (arg->kind == IR_OPERAND_SYMBOL &&
        agg_size_of(slots->aggregates, arg->name) > 8))) {
    load_into(e, slots, arg, dest);
    arm64_emit_word(e, arm64_ldr_imm(1, dest, dest, 0));
    return;
  }
  if (arg->kind == IR_OPERAND_STRING &&
      type_is_cstring(
          call_arg_type(prog, call, (size_t)(arg - call->arguments)))) {
    emit_string_literal_address(e, slots->object, arg->name, dest);
    return;
  }
  if (arg->kind == IR_OPERAND_SYMBOL &&
      arg_is_wide_aggregate(prog, slots, call, arg)) {
    aggregate_address(e, slots, arg, dest);
    return;
  }
  load_into(e, slots, arg, dest);
}

static int emit_call_arguments(Arm64Emit *e, SlotMap *slots,
                               const IRProgram *prog, const StrSet *fs,
                               const IRInstruction *in) {
  if (in->argument_count == 0) return 1;
  const char *who = in->text ? in->text : "(indirect)";
  int count = (int)in->argument_count;
  int *is_float = malloc((size_t)count * sizeof(*is_float));
  Arm64ArgLocation *locations = malloc((size_t)count * sizeof(*locations));
  if (!is_float || !locations) {
    free(is_float);
    free(locations);
    arm64_fail(e, "out of memory laying out the arguments of '%s'", who);
    return 0;
  }
  int *stack_sizes = calloc((size_t)count, sizeof(*stack_sizes));
  if (!stack_sizes) {
    free(is_float);
    free(locations);
    arm64_fail(e, "out of memory laying out the arguments of '%s'", who);
    return 0;
  }
  for (int k = 0; k < count; k++) {
    is_float[k] = call_arg_is_float(prog, in, (size_t)k, fs);
    stack_sizes[k] = stack_size_of_type(call_arg_type(prog, in, (size_t)k));
  }
  if (!arm64_compute_arg_layout_packed(is_float, stack_sizes, count, locations,
                                       NULL)) {
    arm64_fail(e, "cannot lay out the %d arguments of '%s' under AAPCS64",
               count, who);
  }
  for (int k = 0; k < count && !e->error; k++) {
    const IROperand *arg = &in->arguments[k];
    Arm64ArgLocation location = locations[k];
    if (location.kind == ARM64_ARG_IN_VEC_REGISTER) {
      int is_double = call_arg_float_bits(prog, in, (size_t)k) != 32;
      load_float_operand(e, slots, fs, arg, is_double, (int)location.reg,
                         R_LHS);
    } else if (location.kind == ARM64_ARG_IN_GP_REGISTER) {
      load_call_argument(e, slots, prog, in, arg, location.reg);
    } else {
      load_call_argument(e, slots, prog, in, arg, R_LHS);
      arm64_emit_word(e, stack_store_of_size(stack_sizes[k], R_LHS,
                                             location.stack_offset));
    }
  }
  free(stack_sizes);
  free(is_float);
  free(locations);
  return !e->error;
}

typedef struct {
  Arm64Emit *e;
  const IRFunction *fn;
  LblMap *fns;
  const IRProgram *prog;
  const int *retf;
  Arm64ObjectContext *object;
  SlotMap *slots;
  LblMap *labels;
  StrSet *fs;
  StrSet *us;
  AggMap *ag;
  NarrowMap *nm;
  StrSet *wide_params;
  int frame;
  int sret_size;
} Arm64Scope;

static int encode_control_instruction(const Arm64Scope *scope,
                                 const IRInstruction *in) {
  Arm64Emit *e = scope->e;
  const IRFunction *fn = scope->fn;
  LblMap *fns = scope->fns;
  const IRProgram *prog = scope->prog;
  const int *retf = scope->retf;
  Arm64ObjectContext *object = scope->object;
  SlotMap *slots = scope->slots;
  LblMap *labels = scope->labels;
  StrSet *fs = scope->fs;
  StrSet *us = scope->us;
  AggMap *ag = scope->ag;
  NarrowMap *nm = scope->nm;
  StrSet *wide_params = scope->wide_params;
  int frame = scope->frame;
  int sret_size = scope->sret_size;
  (void)fn;
  (void)retf;

  (void)e;
  (void)fn;
  (void)fns;
  (void)prog;
  (void)retf;
  (void)object;
  (void)slots;
  (void)labels;
  (void)fs;
  (void)us;
  (void)ag;
  (void)nm;
  (void)wide_params;
  (void)frame;
  (void)sret_size;
  switch (in->op) {
  case IR_OP_NOP:
    break;
  case IR_OP_DECLARE_LOCAL: {
    const char *name = in->dest.name;
    if (name) {
      const MtlcType *t = declared_type(prog, in);
      int is_scalar_float =
          t ? mtlc_type_is_float(t) : type_text_is_float_scalar(in->text);
      int fbits = t ? (t->kind == MTLC_TYPE_FLOAT32 ? 32 : 64)
                    : type_text_float_bits(in->text);
      set_rebind(fs, name, is_scalar_float, is_scalar_float ? fbits : 0);
      set_rebind(us, name,
                 type_text_is_unsigned(in->text) || type_is_unsigned(t), 0);
      narrow_remove(nm, name);
      if (t && !mtlc_type_is_float(t) && !type_is_aggregate(t)) {
        size_t size = mtlc_type_size(t);
        if (size == 1 || size == 2 || size == 4) {
          narrow_add(nm, name, (int)size, module_type_signed(t));
        }
      }
      agg_remove(ag, name);
      if (type_is_aggregate(t)) {
        agg_add(ag, name, declare_local_size(prog, in));
      }
    }
    break;
  }
  case IR_OP_LABEL:
    arm64_bind_label(e, label_for(e, labels, in->text));
    break;
  case IR_OP_JUMP:
    arm64_emit_b(e, label_for(e, labels, in->text));
    break;
  case IR_OP_BRANCH_ZERO:
    arm64_emit_cbz(e, 1, load_into(e, slots, &in->lhs, R_LHS),
                   label_for(e, labels, in->text));
    break;
  case IR_OP_BRANCH_EQ: {
    Arm64Reg a = load_into(e, slots, &in->lhs, R_LHS);
    Arm64Reg b = load_into(e, slots, &in->rhs, R_RHS);
    arm64_emit_word(e, arm64_cmp_reg(1, a, b));
    arm64_emit_bcond(e, ARM64_EQ, label_for(e, labels, in->text));
    break;
  }
  default:
    return 0;
  }
  return 1;
}

static int encode_assign_instruction(const Arm64Scope *scope,
                                 const IRInstruction *in) {
  Arm64Emit *e = scope->e;
  const IRFunction *fn = scope->fn;
  LblMap *fns = scope->fns;
  const IRProgram *prog = scope->prog;
  const int *retf = scope->retf;
  Arm64ObjectContext *object = scope->object;
  SlotMap *slots = scope->slots;
  LblMap *labels = scope->labels;
  StrSet *fs = scope->fs;
  StrSet *us = scope->us;
  AggMap *ag = scope->ag;
  NarrowMap *nm = scope->nm;
  StrSet *wide_params = scope->wide_params;
  int frame = scope->frame;
  int sret_size = scope->sret_size;
  (void)fn;
  (void)retf;

  (void)e;
  (void)fn;
  (void)fns;
  (void)prog;
  (void)retf;
  (void)object;
  (void)slots;
  (void)labels;
  (void)fs;
  (void)us;
  (void)ag;
  (void)nm;
  (void)wide_params;
  (void)frame;
  (void)sret_size;
  switch (in->op) {
  case IR_OP_ASSIGN: {
    int bytes = in->dest.kind == IR_OPERAND_SYMBOL
                    ? agg_size_of(ag, in->dest.name)
                    : 0;
    int source = in->lhs.kind == IR_OPERAND_SYMBOL
                     ? agg_size_of(ag, in->lhs.name)
                     : 0;
    if (bytes > 8) {
      if (source > 0) {
        emit_lea_local(e, R_LHS, slot_off(slots, in->lhs.name));
      } else {
        load_into(e, slots, &in->lhs, R_LHS);
      }
      emit_lea_local(e, R_RHS, slot_off(slots, in->dest.name));
      emit_fixed_copy(e, R_RHS, R_LHS,
                      source > 0 && source < bytes ? source : bytes);
      break;
    }
    if (bytes > 0 && source > 0) {
      emit_lea_local(e, R_LHS, slot_off(slots, in->lhs.name));
      emit_lea_local(e, R_RHS, slot_off(slots, in->dest.name));
      emit_fixed_copy(e, R_RHS, R_LHS, bytes < source ? bytes : source);
      break;
    }
    if (in->lhs.kind == IR_OPERAND_FLOAT ||
        operand_is_float(fs, &in->lhs)) {
      int dst_bits = 0;
      const IRModuleSymbol *dst_global =
          in->dest.kind == IR_OPERAND_SYMBOL &&
                  slot_find(slots, in->dest.name) < 0
              ? module_variable(slots, in->dest.name)
              : NULL;
      if (dst_global && mtlc_type_is_float(dst_global->type)) {
        dst_bits = dst_global->type->kind == MTLC_TYPE_FLOAT32 ? 32 : 64;
      } else if (in->dest.kind == IR_OPERAND_SYMBOL ||
                 in->dest.kind == IR_OPERAND_TEMP) {
        dst_bits = set_bits(fs, in->dest.name);
      }
      if (dst_bits) {
        int d = dst_bits != 32;
        load_float_operand(e, slots, fs, &in->lhs, d, 0, R_LHS);
        arm64_emit_word(e, arm64_fmov_to_gp(d, R_RES, 0));
        store_dest(e, slots, &in->dest, R_RES);
        break;
      }
    }
    store_dest(e, slots, &in->dest, load_into(e, slots, &in->lhs, R_LHS));
    break;
  }
  default:
    return 0;
  }
  return 1;
}

static int encode_cast_instruction(const Arm64Scope *scope,
                                 const IRInstruction *in) {
  Arm64Emit *e = scope->e;
  const IRFunction *fn = scope->fn;
  LblMap *fns = scope->fns;
  const IRProgram *prog = scope->prog;
  const int *retf = scope->retf;
  Arm64ObjectContext *object = scope->object;
  SlotMap *slots = scope->slots;
  LblMap *labels = scope->labels;
  StrSet *fs = scope->fs;
  StrSet *us = scope->us;
  AggMap *ag = scope->ag;
  NarrowMap *nm = scope->nm;
  StrSet *wide_params = scope->wide_params;
  int frame = scope->frame;
  int sret_size = scope->sret_size;
  (void)fn;
  (void)retf;

  (void)e;
  (void)fn;
  (void)fns;
  (void)prog;
  (void)retf;
  (void)object;
  (void)slots;
  (void)labels;
  (void)fs;
  (void)us;
  (void)ag;
  (void)nm;
  (void)wide_params;
  (void)frame;
  (void)sret_size;
  switch (in->op) {
  case IR_OP_CAST: {
    int srcf = in->is_float;
    int dst_bits = cast_target_float_bits(in);
    int dstf = dst_bits != 0;
    if (srcf && !dstf) {
      int d = operand_float_bits(fs, &in->lhs) != 32;
      int to_u64 = in->text && strcmp(in->text, "uint64") == 0;
      arm64_emit_word(e, arm64_fmov_gp(d, 0, load_into(e, slots, &in->lhs,
                                                       R_LHS)));
      arm64_emit_word(e, to_u64 ? arm64_fcvtzu(d, R_RES, 0)
                                : arm64_fcvtzs(d, R_RES, 0));
      store_dest(e, slots, &in->dest, R_RES);
    } else if (!srcf && dstf) {
      int d = dst_bits != 32;
      Arm64Reg src = load_into(e, slots, &in->lhs, R_LHS);
      arm64_emit_word(e, in->is_unsigned ? arm64_ucvtf(d, 0, src)
                                         : arm64_scvtf(d, 0, src));
      arm64_emit_word(e, arm64_fmov_to_gp(d, R_RES, 0));
      store_dest(e, slots, &in->dest, R_RES);
    } else if (srcf && dstf && cast_small_float_target(in) != 0) {
      int sd = operand_float_bits(fs, &in->lhs) != 32;
      load_float_operand(e, slots, fs, &in->lhs, 0, 0, R_RHS);
      (void)sd;
      arm64_emit_word(e, arm64_fmov_to_gp(0, R_RHS, 0));
      if (cast_small_float_target(in) == 16) {
        emit_f16_narrow(e, R_RHS);
        emit_f16_widen(e, R_RHS);
      } else {
        emit_bf16_narrow(e, R_RHS, R_RES, R_AUX);
        emit_bf16_widen(e, R_RHS);
      }
      store_dest(e, slots, &in->dest, R_RHS);
    } else if (srcf && dstf) {
      int sd = operand_float_bits(fs, &in->lhs) != 32;
      int dd = dst_bits != 32;
      arm64_emit_word(e, arm64_fmov_gp(sd, 0, load_into(e, slots, &in->lhs,
                                                        R_LHS)));
      if (sd != dd) {
        arm64_emit_word(e, arm64_fcvt(dd, 1, 0));
        arm64_emit_word(e, arm64_fmov_to_gp(dd, R_RES, 1));
      } else {
        arm64_emit_word(e, arm64_fmov_to_gp(sd, R_RES, 0));
      }
      store_dest(e, slots, &in->dest, R_RES);
    } else {
      Arm64Reg src = load_into(e, slots, &in->lhs, R_LHS);
      const MtlcType *tt =
          prog && in->text && !strchr(in->text, '[') && !strchr(in->text, '*')
              ? ir_program_lookup_type(prog, in->text)
              : NULL;
      int tw = tt ? (int)mtlc_type_size(tt) : type_elem_size(in->text);
      int tu = tt ? type_is_unsigned(tt) : type_text_is_unsigned(in->text);
      if (tt && type_is_aggregate(tt)) {
        tw = 8;
      }
      if (in->text && strchr(in->text, '*')) {
        tw = 8;
      }
      if (tw == 1 || tw == 2 || tw == 4) {
        arm64_emit_word(
            e, tw == 1 ? (tu ? arm64_uxtb(R_RES, src) : arm64_sxtb(R_RES, src))
               : tw == 2
                   ? (tu ? arm64_uxth(R_RES, src) : arm64_sxth(R_RES, src))
                   : (tu ? arm64_mov_reg(0, R_RES, src)
                         : arm64_sxtw(R_RES, src)));
        store_dest(e, slots, &in->dest, R_RES);
      } else {
        store_dest(e, slots, &in->dest, src);
      }
    }
    break;
  }
  default:
    return 0;
  }
  return 1;
}

static int encode_memory_instruction(const Arm64Scope *scope,
                                 const IRInstruction *in) {
  Arm64Emit *e = scope->e;
  const IRFunction *fn = scope->fn;
  LblMap *fns = scope->fns;
  const IRProgram *prog = scope->prog;
  const int *retf = scope->retf;
  Arm64ObjectContext *object = scope->object;
  SlotMap *slots = scope->slots;
  LblMap *labels = scope->labels;
  StrSet *fs = scope->fs;
  StrSet *us = scope->us;
  AggMap *ag = scope->ag;
  NarrowMap *nm = scope->nm;
  StrSet *wide_params = scope->wide_params;
  int frame = scope->frame;
  int sret_size = scope->sret_size;
  (void)fn;
  (void)retf;

  (void)e;
  (void)fn;
  (void)fns;
  (void)prog;
  (void)retf;
  (void)object;
  (void)slots;
  (void)labels;
  (void)fs;
  (void)us;
  (void)ag;
  (void)nm;
  (void)wide_params;
  (void)frame;
  (void)sret_size;
  switch (in->op) {
  case IR_OP_ADDRESS_OF: {
    const IRModuleSymbol *global =
        slot_find(slots, in->lhs.name) < 0
            ? module_variable(slots, in->lhs.name)
            : NULL;
    if (global) {
      emit_global_address(e, slots, R_RES, global);
    } else if (slot_find(slots, in->lhs.name) < 0 &&
               prog_fn_index(prog, in->lhs.name) >= 0) {
      emit_function_address(e, slots, R_RES, in->lhs.name);
    } else if (set_has(wide_params, in->lhs.name)) {
      emit_slot_ldr(e, R_RES, slot_off(slots, in->lhs.name));
    } else {
      emit_lea_local(e, R_RES, slot_off(slots, in->lhs.name));
    }
    store_dest(e, slots, &in->dest, R_RES);
    break;
  }
  case IR_OP_LOAD: {
    int size = in->rhs.kind == IR_OPERAND_INT ? (int)in->rhs.int_value : 8;
    Arm64Reg addr = address_operand(e, slots, ag, &in->lhs, R_LHS);
    if (size > 8) {
      int dest_agg = in->dest.kind == IR_OPERAND_SYMBOL
                         ? agg_size_of(ag, in->dest.name)
                         : 0;
      if (dest_agg > 8) {
        emit_lea_local(e, R_RHS, slot_off(slots, in->dest.name));
        emit_fixed_copy(e, R_RHS, addr, size < dest_agg ? size : dest_agg);
      } else {
        store_dest(e, slots, &in->dest, addr);
      }
      break;
    }
    emit_load_sized(e, R_RES, addr, size,
                    !in->is_unsigned && !in->is_float);
    if (size == 2 && in->is_float && small_float_class(in) == 16) {
      emit_f16_widen(e, R_RES);
    } else if (size == 2 && in->is_float && small_float_class(in) == 8) {
      emit_bf16_widen(e, R_RES);
    }
    store_dest(e, slots, &in->dest, R_RES);
    break;
  }
  case IR_OP_STORE: {
    int size = in->rhs.kind == IR_OPERAND_INT ? (int)in->rhs.int_value : 8;
    Arm64Reg addr = address_operand(e, slots, ag, &in->dest, R_LHS);
    if (size != 1 && size != 2 && size != 4 && size != 8) {
      Arm64Reg src;
      if (in->lhs.kind == IR_OPERAND_SYMBOL &&
          agg_size_of(ag, in->lhs.name) > 0) {
        emit_lea_local(e, R_RHS, slot_off(slots, in->lhs.name));
        src = R_RHS;
      } else {
        src = load_into(e, slots, &in->lhs, R_RHS);
      }
      emit_fixed_copy(e, addr, src, size);
      break;
    }
    if (size == 2 && in->is_float && small_float_class(in) != 0) {
      load_float_operand(e, slots, fs, &in->lhs, 0, 0, R_RHS);
      arm64_emit_word(e, arm64_fmov_to_gp(0, R_RHS, 0));
      if (small_float_class(in) == 16) {
        emit_f16_narrow(e, R_RHS);
      } else {
        emit_bf16_narrow(e, R_RHS, R_RES, R_AUX);
      }
      emit_store_sized(e, R_RHS, addr, 2);
      break;
    }
    if ((size == 4 || size == 8) && operand_is_float(fs, &in->lhs)) {
      int d = size == 8;
      load_float_operand(e, slots, fs, &in->lhs, d, 0, R_RHS);
      arm64_emit_word(e, arm64_fmov_to_gp(d, R_RHS, 0));
      emit_store_sized(e, R_RHS, addr, size);
      break;
    }
    Arm64Reg val = load_into(e, slots, &in->lhs, R_RHS);
    emit_store_sized(e, val, addr, size);
    break;
  }
  default:
    return 0;
  }
  return 1;
}

static int encode_arithmetic_instruction(const Arm64Scope *scope,
                                 const IRInstruction *in) {
  Arm64Emit *e = scope->e;
  const IRFunction *fn = scope->fn;
  LblMap *fns = scope->fns;
  const IRProgram *prog = scope->prog;
  const int *retf = scope->retf;
  Arm64ObjectContext *object = scope->object;
  SlotMap *slots = scope->slots;
  LblMap *labels = scope->labels;
  StrSet *fs = scope->fs;
  StrSet *us = scope->us;
  AggMap *ag = scope->ag;
  NarrowMap *nm = scope->nm;
  StrSet *wide_params = scope->wide_params;
  int frame = scope->frame;
  int sret_size = scope->sret_size;
  (void)fn;
  (void)retf;

  (void)e;
  (void)fn;
  (void)fns;
  (void)prog;
  (void)retf;
  (void)object;
  (void)slots;
  (void)labels;
  (void)fs;
  (void)us;
  (void)ag;
  (void)nm;
  (void)wide_params;
  (void)frame;
  (void)sret_size;
  switch (in->op) {
  case IR_OP_BINARY:
    lower_binary(e, slots, fs, us, in);
    break;
  case IR_OP_UNARY:
    lower_unary(e, slots, fs, in);
    break;
  default:
    return 0;
  }
  return 1;
}

static int encode_string_concat_instruction(const Arm64Scope *scope,
                                            const IRInstruction *in) {
  if (!binary_is_string_concat(in)) {
    return 0;
  }
  if (!scope->object && !scope->fns) {
    arm64_fail(scope->e, "string concatenation in '%s' needs the runtime "
                         "kernel, which this path does not provide",
               scope->fn->name);
    return 1;
  }
  emit_string_concat(scope->e, scope->slots, scope->ag, scope->object,
                     scope->prog, scope->fns, in);
  return 1;
}

static int encode_call_instruction(const Arm64Scope *scope,
                                 const IRInstruction *in) {
  Arm64Emit *e = scope->e;
  const IRFunction *fn = scope->fn;
  LblMap *fns = scope->fns;
  const IRProgram *prog = scope->prog;
  const int *retf = scope->retf;
  Arm64ObjectContext *object = scope->object;
  SlotMap *slots = scope->slots;
  LblMap *labels = scope->labels;
  StrSet *fs = scope->fs;
  StrSet *us = scope->us;
  AggMap *ag = scope->ag;
  NarrowMap *nm = scope->nm;
  StrSet *wide_params = scope->wide_params;
  int frame = scope->frame;
  int sret_size = scope->sret_size;
  (void)fn;
  (void)retf;

  (void)e;
  (void)fn;
  (void)fns;
  (void)prog;
  (void)retf;
  (void)object;
  (void)slots;
  (void)labels;
  (void)fs;
  (void)us;
  (void)ag;
  (void)nm;
  (void)wide_params;
  (void)frame;
  (void)sret_size;
  switch (in->op) {
  case IR_OP_CALL: {
    if (!fns || !in->text) {
      arm64_fail(e, in->text
                        ? "'%s' calls '%s', but this path lowers one function"
                          " in isolation"
                        : "'%s' contains a call with no callee name%s",
                 fn->name, in->text ? in->text : "");
      break;
    }
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
          arm64_fail(e, "could not place a cstr literal in .rodata");
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
        store_dest(e, slots, &in->dest, R_RES);
      }
      break;
    }
    if (strcmp(in->text, "mettle_crash_trap_ex") == 0 ||
        strcmp(in->text, "mettle_crash_trap") == 0) {
      size_t msg_index =
          strcmp(in->text, "mettle_crash_trap_ex") == 0 ? 1u : 0u;
      if (in->argument_count > msg_index &&
          in->arguments[msg_index].kind == IR_OPERAND_STRING) {
        emit_string_literal_address(e, object,
                                    in->arguments[msg_index].name, ARM64_X0);
      } else if (in->argument_count > msg_index) {
        load_into(e, slots, &in->arguments[msg_index], ARM64_X0);
      } else {
        emit_string_literal_address(e, object, "Fatal error", ARM64_X0);
      }
      if (object) {
        if (!emit_external_call(e, object, prog, "puts")) break;
        emit_imm(e, ARM64_X0, 1);
        if (!emit_external_call(e, object, prog, "exit")) break;
      } else {
        arm64_emit_bl(e, label_for(e, fns, RUNTIME_STUBS[STUB_PUTS].name));
        emit_imm(e, ARM64_X0, 1);
        arm64_emit_word(e, arm64_movz(1, ARM64_X8, SYS_EXIT, 0));
        arm64_emit_word(e, ARM64_SVC0);
      }
      break;
    }
    if (strcmp(in->text, IR_SYSCALL_CALL_NAME) == 0) {
      size_t syscall_arguments =
          in->argument_count > 0 ? in->argument_count - 1 : 0;
      if (in->argument_count == 0 ||
          syscall_arguments > MTLC_SYSCALL_MAX_ARGUMENTS_SVC) {
        arm64_fail(e,
                   "'%s' makes a system call with %llu arguments; this target "
                   "passes at most %d",
                   fn->name, (unsigned long long)syscall_arguments,
                   MTLC_SYSCALL_MAX_ARGUMENTS_SVC);
        break;
      }
      for (size_t k = 0; k < syscall_arguments; k++) {
        load_into(e, slots, &in->arguments[k + 1], (Arm64Reg)(ARM64_X0 + k));
      }
      load_into(e, slots, &in->arguments[0], ARM64_X8);
      arm64_emit_word(e, ARM64_SVC0);
      if (in->dest.kind == IR_OPERAND_TEMP ||
          in->dest.kind == IR_OPERAND_SYMBOL) {
        store_dest(e, slots, &in->dest, ARM64_X0);
      }
      break;
    }

    if (!emit_call_arguments(e, slots, prog, fs, in)) break;

    int callee_sret = fn_aggregate_return_size(prog, in->text);
    if (callee_sret) {
      emit_lea_local(e, ARM64_X8, slot_off(slots, SRET_BUFFER_SLOT));
    }
    int stub = object ? -1 : call_stub_index(prog, in->text);
    if (prog_fn_index(prog, in->text) >= 0) {
      arm64_emit_bl(e, label_for(e, fns, in->text));
    } else if (object) {
      if (!emit_external_call(e, object, prog, in->text)) {
        break;
      }
    } else if (stub >= 0) {
      arm64_emit_bl(e, label_for(e, fns, RUNTIME_STUBS[stub].name));
    } else {
      arm64_fail(e,
                 "'%s' calls '%s', which is neither in this program, a "
                 "runtime stub, nor an external symbol (the self-contained "
                 "ELF has no linker)",
                 fn->name, in->text);
      break;
    }
    if (in->dest.kind == IR_OPERAND_TEMP ||
        in->dest.kind == IR_OPERAND_SYMBOL) {
      int dest_agg = in->dest.kind == IR_OPERAND_SYMBOL
                         ? agg_size_of(ag, in->dest.name)
                         : 0;
      int resf = call_returns_float(prog, in, retf) ||
                 set_has(fs, in->dest.name);
      if (callee_sret && dest_agg > 8) {
        emit_lea_local(e, R_RHS, slot_off(slots, in->dest.name));
        arm64_emit_mov(e, 1, R_LHS, ARM64_X0);
        emit_fixed_copy(e, R_RHS, R_LHS,
                        callee_sret < dest_agg ? callee_sret : dest_agg);
      } else if (resf) {
        int d = call_return_float_bits(prog, in) != 32;
        arm64_emit_word(e, arm64_fmov_to_gp(d, R_RES, 0));
        store_dest(e, slots, &in->dest, R_RES);
      } else {
        store_dest(e, slots, &in->dest, ARM64_X0);
      }
    }
    break;
  }
  default:
    return 0;
  }
  return 1;
}

static int encode_runtime_instruction(const Arm64Scope *scope,
                                 const IRInstruction *in) {
  Arm64Emit *e = scope->e;
  const IRFunction *fn = scope->fn;
  LblMap *fns = scope->fns;
  const IRProgram *prog = scope->prog;
  const int *retf = scope->retf;
  Arm64ObjectContext *object = scope->object;
  SlotMap *slots = scope->slots;
  LblMap *labels = scope->labels;
  StrSet *fs = scope->fs;
  StrSet *us = scope->us;
  AggMap *ag = scope->ag;
  NarrowMap *nm = scope->nm;
  StrSet *wide_params = scope->wide_params;
  int frame = scope->frame;
  int sret_size = scope->sret_size;
  (void)fn;
  (void)retf;

  (void)e;
  (void)fn;
  (void)fns;
  (void)prog;
  (void)retf;
  (void)object;
  (void)slots;
  (void)labels;
  (void)fs;
  (void)us;
  (void)ag;
  (void)nm;
  (void)wide_params;
  (void)frame;
  (void)sret_size;
  switch (in->op) {
  case IR_OP_CALL_INDIRECT: {
    if (!emit_call_arguments(e, slots, prog, fs, in)) break;
    load_into(e, slots, &in->lhs, ARM64_X16);
    arm64_emit_word(e, arm64_blr(ARM64_X16));
    if (in->dest.kind == IR_OPERAND_TEMP ||
        in->dest.kind == IR_OPERAND_SYMBOL) {
      if (call_returns_float(prog, in, retf) || set_has(fs, in->dest.name)) {
        int d = call_return_float_bits(prog, in) != 32;
        arm64_emit_word(e, arm64_fmov_to_gp(d, R_RES, 0));
        store_dest(e, slots, &in->dest, R_RES);
      } else {
        store_dest(e, slots, &in->dest, ARM64_X0);
      }
    }
    break;
  }
  case IR_OP_NEW: {
    Arm64Reg size_reg = object ? ARM64_X1 : ARM64_X0;
    if (in->rhs.kind == IR_OPERAND_NONE ||
        (in->rhs.kind == IR_OPERAND_INT && in->rhs.int_value <= 0)) {
      emit_imm(e, size_reg, 8);
    } else if (in->rhs.kind == IR_OPERAND_INT) {
      emit_imm(e, size_reg, (uint64_t)in->rhs.int_value);
    } else {
      load_into(e, slots, &in->rhs, size_reg);
    }
    if (object) {
      emit_imm(e, ARM64_X0, 1);
      if (!emit_external_call(e, object, prog, "calloc")) break;
    } else if (fns) {
      arm64_emit_bl(e, label_for(e, fns, RUNTIME_STUBS[STUB_MALLOC].name));
    } else {
      arm64_fail(e, "'%s' allocates, but this path lowers one function in "
                    "isolation", fn->name);
      break;
    }
    if (in->dest.kind == IR_OPERAND_TEMP ||
        in->dest.kind == IR_OPERAND_SYMBOL) {
      store_dest(e, slots, &in->dest, ARM64_X0);
    }
    break;
  }
  case IR_OP_RETURN:
    if (sret_size && in->lhs.kind != IR_OPERAND_NONE) {
      if (in->lhs.kind == IR_OPERAND_SYMBOL &&
          agg_size_of(ag, in->lhs.name) > 0) {
        emit_lea_local(e, R_LHS, slot_off(slots, in->lhs.name));
      } else {
        load_into(e, slots, &in->lhs, R_LHS);
      }
      emit_slot_ldr(e, R_RHS, slot_off(slots, SRET_SAVE_SLOT));
      emit_fixed_copy(e, R_RHS, R_LHS, sret_size);
      arm64_emit_mov(e, 1, ARM64_X0, R_RHS);
      arm64_emit_epilogue(e, frame, NULL, 0);
      break;
    }
    if (in->lhs.kind != IR_OPERAND_NONE) {
      if (operand_is_float(fs, &in->lhs)) {
        const IRModuleSymbol *current = module_function(prog, fn->name);
        int d = current && current->return_type
                    ? current->return_type->kind != MTLC_TYPE_FLOAT32
                    : operand_float_bits(fs, &in->lhs) != 32;
        load_float_operand(e, slots, fs, &in->lhs, d, 0, R_LHS);
      } else {
        arm64_emit_mov(e, 1, ARM64_X0, load_into(e, slots, &in->lhs, R_LHS));
      }
    }
    arm64_emit_epilogue(e, frame, NULL, 0);
    break;
  default:
    return 0;
  }
  return 1;
}

static void encode_instruction(const Arm64Scope *scope,
                               const IRInstruction *in) {
  if (encode_control_instruction(scope, in) ||
      encode_assign_instruction(scope, in) ||
      encode_cast_instruction(scope, in) ||
      encode_memory_instruction(scope, in) ||
      encode_arithmetic_instruction(scope, in) ||
      encode_string_concat_instruction(scope, in) ||
      encode_call_instruction(scope, in) ||
      encode_runtime_instruction(scope, in)) {
    return;
  }
  arm64_fail(scope->e, "'%s' uses %s, which has no AArch64 lowering",
             scope->fn->name, ir_opcode_name(in->op));
}

static int encode_function(Arm64Emit *e, const IRFunction *fn, LblMap *fns,
                           const IRProgram *prog, const int *retf,
                           Arm64ObjectContext *object, Arm64Data *data) {
  SlotMap slots = {0};
  LblMap labels = {0};
  StrSet fs = {0};
  StrSet us = {0};
  AggMap ag = {0};
  NarrowMap nm = {0};
  build_float_set(fn, prog, retf, &fs);
  build_unsigned_set(fn, &us);
  build_aggregate_set(fn, prog, &ag);
  build_narrow_map(fn, prog, &nm);
  slots.object = object;
  slots.program = prog;
  slots.data = data;
  slots.fns = fns;
  slots.aggregates = &ag;
  slots.narrow = &nm;
  slots.frame = max_outgoing_stack(e, fn, prog, &fs);

  for (size_t i = 0; i < fn->parameter_count; i++) {
    slot_alloc(&slots, fn->parameter_names[i], 8);
  }
  for (size_t i = 0; i < fn->instruction_count; i++) {
    const IRInstruction *in = &fn->instructions[i];
    if (in->op == IR_OP_DECLARE_LOCAL && in->dest.kind == IR_OPERAND_SYMBOL &&
        in->dest.name) {
      slot_alloc(&slots, in->dest.name, declare_local_size(prog, in));
    }
  }
  for (size_t i = 0; i < fn->instruction_count; i++) {
    const IRInstruction *in = &fn->instructions[i];
    const IROperand *ops[3] = {&in->dest, &in->lhs, &in->rhs};
    for (int k = 0; k < 3; k++) {
      if (operand_needs_slot(&slots, prog, ops[k])) {
        slot_off(&slots, ops[k]->name);
      }
    }
    for (size_t k = 0; k < in->argument_count; k++) {
      if (operand_needs_slot(&slots, prog, &in->arguments[k])) {
        slot_off(&slots, in->arguments[k].name);
      }
    }
  }

  StrSet wide_params = {0};
  {
    const IRModuleSymbol *self = module_function(prog, fn->name);
    for (size_t i = 0; self && i < fn->parameter_count; i++) {
      if (i < self->param_count && self->param_types &&
          type_is_aggregate(self->param_types[i]) &&
          mtlc_type_size(self->param_types[i]) > 8) {
        set_add(&wide_params, fn->parameter_names[i]);
      }
    }
  }

  int sret_size = fns ? fn_aggregate_return_size(prog, fn->name) : 0;
  if (sret_size) {
    slot_alloc(&slots, SRET_SAVE_SLOT, 8);
  }
  int sret_buffer = 0;
  for (size_t i = 0; i < fn->instruction_count; i++) {
    const IRInstruction *in = &fn->instructions[i];
    if (in->op == IR_OP_CALL) {
      int ret = fn_aggregate_return_size(prog, in->text);
      if (ret > sret_buffer) {
        sret_buffer = ret;
      }
    }
  }
  if (sret_buffer) {
    slot_alloc(&slots, SRET_BUFFER_SLOT, sret_buffer);
  }
  for (size_t i = 0; i < fn->instruction_count; i++) {
    if (binary_is_string_concat(&fn->instructions[i])) {
      slot_alloc(&slots, CONCAT_A_SLOT, 8);
      slot_alloc(&slots, CONCAT_B_SLOT, 8);
      slot_alloc(&slots, CONCAT_DST_SLOT, 8);
      break;
    }
  }

  int frame = (slots.frame + 15) & ~15;
  if (e->error || !arm64_emit_prologue(e, frame, NULL, 0)) {
    goto done;
  }
  if (sret_size) {
    emit_slot_str(e, ARM64_X8, slot_off(&slots, SRET_SAVE_SLOT));
  }
  if (fn->parameter_count > 0) {
    int count = (int)fn->parameter_count;
    int *is_float = malloc((size_t)count * sizeof(*is_float));
    Arm64ArgLocation *locations =
        malloc((size_t)count * sizeof(*locations));
    if (!is_float || !locations) {
      free(is_float);
      free(locations);
      arm64_fail(e, "out of memory homing the parameters of '%s'", fn->name);
      goto done;
    }
    int *stack_sizes = calloc((size_t)count, sizeof(*stack_sizes));
    if (!stack_sizes) {
      free(is_float);
      free(locations);
      arm64_fail(e, "out of memory homing the parameters of '%s'", fn->name);
      goto done;
    }
    for (int i = 0; i < count; i++) {
      const char *type = fn->parameter_types ? fn->parameter_types[i] : NULL;
      is_float[i] = type_text_is_float_scalar(type);
      stack_sizes[i] = stack_size_of_type_text(type);
    }
    if (!arm64_compute_arg_layout_packed(is_float, stack_sizes, count,
                                         locations, NULL)) {
      arm64_fail(e, "cannot lay out the %d parameters of '%s' under AAPCS64",
                 count, fn->name);
    }
    for (int i = 0; i < count && !e->error; i++) {
      int off = slot_off(&slots, fn->parameter_names[i]);
      Arm64ArgLocation location = locations[i];
      if (location.kind == ARM64_ARG_IN_VEC_REGISTER) {
        const char *type =
            fn->parameter_types ? fn->parameter_types[i] : NULL;
        int is_double = type_text_float_bits(type) == 64;
        emit_slot_str_fp(e, is_double, (int)location.reg, off);
      } else if (location.kind == ARM64_ARG_IN_GP_REGISTER) {
        emit_slot_str(e, location.reg, off);
      } else {
        const char *type =
            fn->parameter_types ? fn->parameter_types[i] : NULL;
        int from = 16 + location.stack_offset;
        switch (stack_sizes[i]) {
        case 1:
          arm64_emit_word(e, arm64_ldrb_imm(R_LHS, ARM64_X29, from));
          if (type_text_base_is(type, "int8")) {
            arm64_emit_word(e, arm64_sxtb(R_LHS, R_LHS));
          }
          break;
        case 2:
          arm64_emit_word(e, arm64_ldrh_imm(R_LHS, ARM64_X29, from));
          if (type_text_base_is(type, "int16")) {
            arm64_emit_word(e, arm64_sxth(R_LHS, R_LHS));
          }
          break;
        case 4:
          arm64_emit_word(e, arm64_ldr_imm(0, R_LHS, ARM64_X29, from));
          if (type_text_base_is(type, "int32")) {
            arm64_emit_word(e, arm64_sxtw(R_LHS, R_LHS));
          }
          break;
        default:
          arm64_emit_word(e, arm64_ldr_imm(1, R_LHS, ARM64_X29, from));
          break;
        }
        emit_slot_str(e, R_LHS, off);
      }
    }
    free(stack_sizes);
    free(is_float);
    free(locations);
    if (e->error) goto done;
  }

  {
    Arm64Scope scope;
    scope.e = e;
    scope.fn = fn;
    scope.fns = fns;
    scope.prog = prog;
    scope.retf = retf;
    scope.object = object;
    scope.slots = &slots;
    scope.labels = &labels;
    scope.fs = &fs;
    scope.us = &us;
    scope.ag = &ag;
    scope.nm = &nm;
    scope.wide_params = &wide_params;
    scope.frame = frame;
    scope.sret_size = sret_size;
    for (size_t i = 0; i < fn->instruction_count && !e->error; i++) {
      encode_instruction(&scope, &fn->instructions[i]);
    }
  }

done:
  free(slots.names);
  free(slots.offs);
  free(labels.names);
  free(labels.ids);
  free(fs.names);
  free(fs.bits);
  free(us.names);
  free(ag.names);
  free(ag.sizes);
  free(nm.names);
  free(nm.sizes);
  free(nm.is_signed);
  free(wide_params.names);
  free(wide_params.bits);
  return e->error ? 0 : 1;
}

int arm64_ir_encode_function(Arm64Emit *e, const IRFunction *fn) {
  return encode_function(e, fn, NULL, NULL, NULL, NULL, NULL);
}

static int io_stub_intrinsic(const char *name, int *with_newline,
                             int *is_string) {
  if (!name) {
    return 0;
  }
  struct {
    const char *n;
    int nl, str;
  } table[] = {{"println", 1, 1}, {"print", 0, 1}};
  for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
    if (strcmp(name, table[i].n) == 0) {
      if (with_newline) *with_newline = table[i].nl;
      if (is_string) *is_string = table[i].str;
      return 1;
    }
  }
  return 0;
}

static int io_leaf(const char *name) {
  return io_stub_intrinsic(name, NULL, NULL) ||
         (name && strcmp(name, "cstr") == 0);
}

static void emit_str_print(Arm64Emit *e, int with_newline, int zero_result) {
  int l_scan = arm64_new_label(e);
  int l_write = arm64_new_label(e);
  arm64_emit_prologue(e, 16, NULL, 0);
  arm64_emit_word(e, arm64_mov_reg(1, ARM64_X9, ARM64_X0));
  arm64_emit_word(e, arm64_movz(1, ARM64_X10, 0, 0));
  arm64_bind_label(e, l_scan);
  arm64_emit_word(e, arm64_ldrb_imm(ARM64_X11, ARM64_X9, 0));
  arm64_emit_cbz(e, 0, ARM64_X11, l_write);
  arm64_emit_word(e, arm64_add_imm(1, ARM64_X9, ARM64_X9, 1, 0));
  arm64_emit_word(e, arm64_add_imm(1, ARM64_X10, ARM64_X10, 1, 0));
  arm64_emit_b(e, l_scan);
  arm64_bind_label(e, l_write);
  arm64_emit_word(e, arm64_mov_reg(1, ARM64_X1, ARM64_X0));
  arm64_emit_word(e, arm64_mov_reg(1, ARM64_X2, ARM64_X10));
  arm64_emit_word(e, arm64_movz(1, ARM64_X0, 1, 0));
  arm64_emit_word(e, arm64_movz(1, ARM64_X8, SYS_WRITE, 0));
  arm64_emit_word(e, ARM64_SVC0);
  if (with_newline) {
    arm64_emit_word(e, arm64_movz(1, ARM64_X11, 10, 0));
    arm64_emit_word(e, arm64_strb_imm(ARM64_X11, ARM64_SP, 0));
    arm64_emit_word(e, arm64_mov_sp(ARM64_X1, ARM64_SP));
    arm64_emit_word(e, arm64_movz(1, ARM64_X2, 1, 0));
    arm64_emit_word(e, arm64_movz(1, ARM64_X0, 1, 0));
    arm64_emit_word(e, arm64_movz(1, ARM64_X8, SYS_WRITE, 0));
    arm64_emit_word(e, ARM64_SVC0);
  }
  if (zero_result) {
    arm64_emit_word(e, arm64_movz(1, ARM64_X0, 0, 0));
  }
  arm64_emit_epilogue(e, 16, NULL, 0);
}

static void emit_stub_malloc(Arm64Emit *e) {
  int l_grow = arm64_new_label(e);
  int l_fits = arm64_new_label(e);
  int l_oom = arm64_new_label(e);

  arm64_emit_prologue(e, 0, NULL, 0);
  arm64_emit_word(e, arm64_add_imm(1, ARM64_X9, ARM64_X0, 15, 0));
  arm64_emit_word(e, arm64_movn(1, ARM64_X10, 15, 0));
  arm64_emit_word(e, arm64_and_reg(1, ARM64_X9, ARM64_X9, ARM64_X10));

  emit_imm(e, ARM64_X11, (uint64_t)ELF_DATA_VADDR);
  arm64_emit_word(e, arm64_ldr_imm(1, ARM64_X12, ARM64_X11, HEAP_CURSOR_OFF));
  arm64_emit_word(e, arm64_ldr_imm(1, ARM64_X13, ARM64_X11, HEAP_END_OFF));
  arm64_emit_word(e, arm64_add_reg(1, ARM64_X14, ARM64_X12, ARM64_X9));
  arm64_emit_word(e, arm64_cmp_reg(1, ARM64_X14, ARM64_X13));
  arm64_emit_bcond(e, ARM64_LS, l_fits);

  arm64_bind_label(e, l_grow);
  emit_imm(e, ARM64_X15, HEAP_ARENA_BYTES);
  arm64_emit_word(e, arm64_add_reg(1, ARM64_X15, ARM64_X15, ARM64_X9));
  arm64_emit_word(e, arm64_movz(1, ARM64_X0, 0, 0));
  arm64_emit_mov(e, 1, ARM64_X1, ARM64_X15);
  arm64_emit_word(e, arm64_movz(1, ARM64_X2, 3, 0));
  arm64_emit_word(e, arm64_movz(1, ARM64_X3, 0x22, 0));
  arm64_emit_word(e, arm64_movn(1, ARM64_X4, 0, 0));
  arm64_emit_word(e, arm64_movz(1, ARM64_X5, 0, 0));
  arm64_emit_word(e, arm64_movz(1, ARM64_X8, SYS_MMAP, 0));
  arm64_emit_word(e, ARM64_SVC0);
  arm64_emit_word(e, arm64_cmp_imm(1, ARM64_X0, 0, 0));
  arm64_emit_bcond(e, ARM64_LT, l_oom);
  arm64_emit_mov(e, 1, ARM64_X12, ARM64_X0);
  arm64_emit_word(e, arm64_add_reg(1, ARM64_X13, ARM64_X0, ARM64_X15));
  arm64_emit_word(e, arm64_add_reg(1, ARM64_X14, ARM64_X12, ARM64_X9));
  emit_imm(e, ARM64_X11, (uint64_t)ELF_DATA_VADDR);
  arm64_emit_word(e, arm64_str_imm(1, ARM64_X13, ARM64_X11, HEAP_END_OFF));

  arm64_bind_label(e, l_fits);
  arm64_emit_word(e, arm64_str_imm(1, ARM64_X14, ARM64_X11, HEAP_CURSOR_OFF));
  arm64_emit_mov(e, 1, ARM64_X0, ARM64_X12);
  arm64_emit_epilogue(e, 0, NULL, 0);

  arm64_bind_label(e, l_oom);
  arm64_emit_word(e, arm64_movz(1, ARM64_X0, 0, 0));
  arm64_emit_epilogue(e, 0, NULL, 0);
}

static void emit_stub_calloc(Arm64Emit *e, int malloc_label) {
  arm64_emit_prologue(e, 0, NULL, 0);
  arm64_emit_word(e, arm64_mul(1, ARM64_X0, ARM64_X0, ARM64_X1));
  arm64_emit_bl(e, malloc_label);
  arm64_emit_epilogue(e, 0, NULL, 0);
}

static void emit_stub_free(Arm64Emit *e) {
  arm64_emit_prologue(e, 0, NULL, 0);
  arm64_emit_epilogue(e, 0, NULL, 0);
}

static void emit_stub_putchar(Arm64Emit *e) {
  arm64_emit_prologue(e, 16, NULL, 0);
  arm64_emit_word(e, arm64_strb_imm(ARM64_X0, ARM64_SP, 0));
  arm64_emit_mov(e, 1, ARM64_X9, ARM64_X0);
  arm64_emit_mov(e, 1, ARM64_X1, ARM64_SP);
  arm64_emit_word(e, arm64_movz(1, ARM64_X2, 1, 0));
  arm64_emit_word(e, arm64_movz(1, ARM64_X0, 1, 0));
  arm64_emit_word(e, arm64_movz(1, ARM64_X8, SYS_WRITE, 0));
  arm64_emit_word(e, ARM64_SVC0);
  arm64_emit_mov(e, 1, ARM64_X0, ARM64_X9);
  arm64_emit_epilogue(e, 16, NULL, 0);
}

static void emit_stub_write(Arm64Emit *e) {
  arm64_emit_prologue(e, 0, NULL, 0);
  arm64_emit_word(e, arm64_movz(1, ARM64_X8, SYS_WRITE, 0));
  arm64_emit_word(e, ARM64_SVC0);
  arm64_emit_epilogue(e, 0, NULL, 0);
}

static void emit_stub_strlen(Arm64Emit *e) {
  int l_scan = arm64_new_label(e);
  int l_done = arm64_new_label(e);
  arm64_emit_prologue(e, 0, NULL, 0);
  arm64_emit_mov(e, 1, ARM64_X9, ARM64_X0);
  arm64_emit_word(e, arm64_movz(1, ARM64_X10, 0, 0));
  arm64_bind_label(e, l_scan);
  arm64_emit_word(e, arm64_ldrb_imm(ARM64_X11, ARM64_X9, 0));
  arm64_emit_cbz(e, 0, ARM64_X11, l_done);
  arm64_emit_word(e, arm64_add_imm(1, ARM64_X9, ARM64_X9, 1, 0));
  arm64_emit_word(e, arm64_add_imm(1, ARM64_X10, ARM64_X10, 1, 0));
  arm64_emit_b(e, l_scan);
  arm64_bind_label(e, l_done);
  arm64_emit_mov(e, 1, ARM64_X0, ARM64_X10);
  arm64_emit_epilogue(e, 0, NULL, 0);
}

static void emit_stub_memcpy(Arm64Emit *e) {
  int l_loop = arm64_new_label(e);
  int l_done = arm64_new_label(e);
  arm64_emit_prologue(e, 0, NULL, 0);
  arm64_emit_mov(e, 1, ARM64_X9, ARM64_X0);
  arm64_emit_cbz(e, 1, ARM64_X2, l_done);
  arm64_bind_label(e, l_loop);
  arm64_emit_word(e, arm64_ldrb_imm(ARM64_X11, ARM64_X1, 0));
  arm64_emit_word(e, arm64_strb_imm(ARM64_X11, ARM64_X9, 0));
  arm64_emit_word(e, arm64_add_imm(1, ARM64_X1, ARM64_X1, 1, 0));
  arm64_emit_word(e, arm64_add_imm(1, ARM64_X9, ARM64_X9, 1, 0));
  arm64_emit_word(e, arm64_sub_imm(1, ARM64_X2, ARM64_X2, 1, 0));
  arm64_emit_cbnz(e, 1, ARM64_X2, l_loop);
  arm64_bind_label(e, l_done);
  arm64_emit_epilogue(e, 0, NULL, 0);
}

static void emit_stub_memmove(Arm64Emit *e) {
  int l_fwd = arm64_new_label(e);
  int l_bloop = arm64_new_label(e);
  int l_floop = arm64_new_label(e);
  int l_done = arm64_new_label(e);
  arm64_emit_prologue(e, 0, NULL, 0);
  arm64_emit_cbz(e, 1, ARM64_X2, l_done);
  arm64_emit_word(e, arm64_cmp_reg(1, ARM64_X0, ARM64_X1));
  arm64_emit_bcond(e, ARM64_LS, l_fwd);
  arm64_emit_word(e, arm64_add_reg(1, ARM64_X9, ARM64_X0, ARM64_X2));
  arm64_emit_word(e, arm64_add_reg(1, ARM64_X10, ARM64_X1, ARM64_X2));
  arm64_bind_label(e, l_bloop);
  arm64_emit_word(e, arm64_sub_imm(1, ARM64_X9, ARM64_X9, 1, 0));
  arm64_emit_word(e, arm64_sub_imm(1, ARM64_X10, ARM64_X10, 1, 0));
  arm64_emit_word(e, arm64_ldrb_imm(ARM64_X11, ARM64_X10, 0));
  arm64_emit_word(e, arm64_strb_imm(ARM64_X11, ARM64_X9, 0));
  arm64_emit_word(e, arm64_sub_imm(1, ARM64_X2, ARM64_X2, 1, 0));
  arm64_emit_cbnz(e, 1, ARM64_X2, l_bloop);
  arm64_emit_b(e, l_done);
  arm64_bind_label(e, l_fwd);
  arm64_emit_mov(e, 1, ARM64_X9, ARM64_X0);
  arm64_bind_label(e, l_floop);
  arm64_emit_word(e, arm64_ldrb_imm(ARM64_X11, ARM64_X1, 0));
  arm64_emit_word(e, arm64_strb_imm(ARM64_X11, ARM64_X9, 0));
  arm64_emit_word(e, arm64_add_imm(1, ARM64_X1, ARM64_X1, 1, 0));
  arm64_emit_word(e, arm64_add_imm(1, ARM64_X9, ARM64_X9, 1, 0));
  arm64_emit_word(e, arm64_sub_imm(1, ARM64_X2, ARM64_X2, 1, 0));
  arm64_emit_cbnz(e, 1, ARM64_X2, l_floop);
  arm64_bind_label(e, l_done);
  arm64_emit_epilogue(e, 0, NULL, 0);
}

static void emit_stub_memset(Arm64Emit *e) {
  int l_loop = arm64_new_label(e);
  int l_done = arm64_new_label(e);
  arm64_emit_prologue(e, 0, NULL, 0);
  arm64_emit_mov(e, 1, ARM64_X9, ARM64_X0);
  arm64_emit_cbz(e, 1, ARM64_X2, l_done);
  arm64_bind_label(e, l_loop);
  arm64_emit_word(e, arm64_strb_imm(ARM64_X1, ARM64_X9, 0));
  arm64_emit_word(e, arm64_add_imm(1, ARM64_X9, ARM64_X9, 1, 0));
  arm64_emit_word(e, arm64_sub_imm(1, ARM64_X2, ARM64_X2, 1, 0));
  arm64_emit_cbnz(e, 1, ARM64_X2, l_loop);
  arm64_bind_label(e, l_done);
  arm64_emit_epilogue(e, 0, NULL, 0);
}

static void emit_string_from_value(Arm64Emit *e, int malloc_label,
                                   int is_signed) {
  int l_pos = arm64_new_label(e);
  int l_loop = arm64_new_label(e);
  int l_sign = arm64_new_label(e);
  int l_done = arm64_new_label(e);

  arm64_emit_prologue(e, 16, NULL, 0);
  arm64_emit_word(e, arm64_str_imm(1, ARM64_X0, ARM64_SP, 0));
  arm64_emit_word(e, arm64_movz(1, ARM64_X0, 48, 0));
  arm64_emit_bl(e, malloc_label);
  arm64_emit_word(e, arm64_ldr_imm(1, ARM64_X9, ARM64_SP, 0));
  arm64_emit_word(e, arm64_add_imm(1, ARM64_X10, ARM64_X0, 46, 0));
  arm64_emit_word(e, arm64_movz(1, ARM64_X12, 0, 0));
  if (is_signed) {
    arm64_emit_word(e, arm64_cmp_imm(1, ARM64_X9, 0, 0));
    arm64_emit_bcond(e, ARM64_GE, l_pos);
    arm64_emit_word(e, arm64_movz(1, ARM64_X12, 1, 0));
    arm64_emit_word(e, arm64_neg(1, ARM64_X9, ARM64_X9));
  }
  arm64_bind_label(e, l_pos);
  arm64_emit_cbnz(e, 1, ARM64_X9, l_loop);
  arm64_emit_word(e, arm64_movz(1, ARM64_X11, 48, 0));
  arm64_emit_word(e, arm64_strb_imm(ARM64_X11, ARM64_X10, 0));
  arm64_emit_word(e, arm64_sub_imm(1, ARM64_X10, ARM64_X10, 1, 0));
  arm64_emit_b(e, l_sign);
  arm64_bind_label(e, l_loop);
  arm64_emit_cbz(e, 1, ARM64_X9, l_sign);
  arm64_emit_word(e, arm64_movz(1, ARM64_X13, 10, 0));
  arm64_emit_word(e, arm64_udiv(1, ARM64_X14, ARM64_X9, ARM64_X13));
  arm64_emit_word(e, arm64_msub(1, ARM64_X15, ARM64_X14, ARM64_X13, ARM64_X9));
  arm64_emit_word(e, arm64_mov_reg(1, ARM64_X9, ARM64_X14));
  arm64_emit_word(e, arm64_add_imm(1, ARM64_X15, ARM64_X15, 48, 0));
  arm64_emit_word(e, arm64_strb_imm(ARM64_X15, ARM64_X10, 0));
  arm64_emit_word(e, arm64_sub_imm(1, ARM64_X10, ARM64_X10, 1, 0));
  arm64_emit_b(e, l_loop);
  arm64_bind_label(e, l_sign);
  arm64_emit_cbz(e, 1, ARM64_X12, l_done);
  arm64_emit_word(e, arm64_movz(1, ARM64_X11, 45, 0));
  arm64_emit_word(e, arm64_strb_imm(ARM64_X11, ARM64_X10, 0));
  arm64_emit_word(e, arm64_sub_imm(1, ARM64_X10, ARM64_X10, 1, 0));
  arm64_bind_label(e, l_done);
  arm64_emit_word(e, arm64_add_imm(1, ARM64_X13, ARM64_X10, 1, 0));
  arm64_emit_word(e, arm64_str_imm(1, ARM64_X13, ARM64_X0, 0));
  arm64_emit_word(e, arm64_add_imm(1, ARM64_X11, ARM64_X0, 46, 0));
  arm64_emit_word(e, arm64_sub_reg(1, ARM64_X11, ARM64_X11, ARM64_X10));
  arm64_emit_word(e, arm64_str_imm(1, ARM64_X11, ARM64_X0, 8));
  arm64_emit_epilogue(e, 16, NULL, 0);
}

static void emit_string_from_char(Arm64Emit *e, int malloc_label) {
  arm64_emit_prologue(e, 16, NULL, 0);
  arm64_emit_word(e, arm64_str_imm(1, ARM64_X0, ARM64_SP, 0));
  arm64_emit_word(e, arm64_movz(1, ARM64_X0, 32, 0));
  arm64_emit_bl(e, malloc_label);
  arm64_emit_word(e, arm64_ldr_imm(1, ARM64_X9, ARM64_SP, 0));
  arm64_emit_word(e, arm64_strb_imm(ARM64_X9, ARM64_X0, 16));
  arm64_emit_word(e, arm64_add_imm(1, ARM64_X13, ARM64_X0, 16, 0));
  arm64_emit_word(e, arm64_str_imm(1, ARM64_X13, ARM64_X0, 0));
  arm64_emit_word(e, arm64_movz(1, ARM64_X12, 1, 0));
  arm64_emit_word(e, arm64_str_imm(1, ARM64_X12, ARM64_X0, 8));
  arm64_emit_epilogue(e, 16, NULL, 0);
}

static void emit_string_eq(Arm64Emit *e) {
  int l_loop = arm64_new_label(e);
  int l_equal = arm64_new_label(e);
  int l_differ = arm64_new_label(e);

  arm64_emit_prologue(e, 0, NULL, 0);
  arm64_emit_word(e, arm64_ldr_imm(1, ARM64_X9, ARM64_X0, 8));
  arm64_emit_word(e, arm64_ldr_imm(1, ARM64_X10, ARM64_X1, 8));
  arm64_emit_word(e, arm64_cmp_reg(1, ARM64_X9, ARM64_X10));
  arm64_emit_bcond(e, ARM64_NE, l_differ);
  arm64_emit_word(e, arm64_ldr_imm(1, ARM64_X11, ARM64_X0, 0));
  arm64_emit_word(e, arm64_ldr_imm(1, ARM64_X12, ARM64_X1, 0));
  arm64_bind_label(e, l_loop);
  arm64_emit_cbz(e, 1, ARM64_X9, l_equal);
  arm64_emit_word(e, arm64_ldrb_imm(ARM64_X13, ARM64_X11, 0));
  arm64_emit_word(e, arm64_ldrb_imm(ARM64_X14, ARM64_X12, 0));
  arm64_emit_word(e, arm64_cmp_reg(0, ARM64_X13, ARM64_X14));
  arm64_emit_bcond(e, ARM64_NE, l_differ);
  arm64_emit_word(e, arm64_add_imm(1, ARM64_X11, ARM64_X11, 1, 0));
  arm64_emit_word(e, arm64_add_imm(1, ARM64_X12, ARM64_X12, 1, 0));
  arm64_emit_word(e, arm64_sub_imm(1, ARM64_X9, ARM64_X9, 1, 0));
  arm64_emit_b(e, l_loop);
  arm64_bind_label(e, l_equal);
  arm64_emit_word(e, arm64_movz(1, ARM64_X0, 1, 0));
  arm64_emit_epilogue(e, 0, NULL, 0);
  arm64_bind_label(e, l_differ);
  arm64_emit_word(e, arm64_movz(1, ARM64_X0, 0, 0));
  arm64_emit_epilogue(e, 0, NULL, 0);
}

static void emit_string_from_bool(Arm64Emit *e, int malloc_label) {
  int l_true = arm64_new_label(e);
  int l_store = arm64_new_label(e);
  static const char k_false[] = "false";
  static const char k_true[] = "true";

  arm64_emit_prologue(e, 16, NULL, 0);
  arm64_emit_word(e, arm64_str_imm(1, ARM64_X0, ARM64_SP, 0));
  arm64_emit_word(e, arm64_movz(1, ARM64_X0, 32, 0));
  arm64_emit_bl(e, malloc_label);
  arm64_emit_word(e, arm64_ldr_imm(1, ARM64_X9, ARM64_SP, 0));
  arm64_emit_cbnz(e, 1, ARM64_X9, l_true);
  for (int i = 0; i < 5; i++) {
    arm64_emit_word(e, arm64_movz(1, ARM64_X11, (uint16_t)k_false[i], 0));
    arm64_emit_word(e, arm64_strb_imm(ARM64_X11, ARM64_X0, 16 + i));
  }
  arm64_emit_word(e, arm64_movz(1, ARM64_X12, 5, 0));
  arm64_emit_b(e, l_store);
  arm64_bind_label(e, l_true);
  for (int i = 0; i < 4; i++) {
    arm64_emit_word(e, arm64_movz(1, ARM64_X11, (uint16_t)k_true[i], 0));
    arm64_emit_word(e, arm64_strb_imm(ARM64_X11, ARM64_X0, 16 + i));
  }
  arm64_emit_word(e, arm64_movz(1, ARM64_X12, 4, 0));
  arm64_bind_label(e, l_store);
  arm64_emit_word(e, arm64_add_imm(1, ARM64_X13, ARM64_X0, 16, 0));
  arm64_emit_word(e, arm64_str_imm(1, ARM64_X13, ARM64_X0, 0));
  arm64_emit_word(e, arm64_str_imm(1, ARM64_X12, ARM64_X0, 8));
  arm64_emit_epilogue(e, 16, NULL, 0);
}

static void emit_stub_exit(Arm64Emit *e, int fixed_status) {
  if (fixed_status >= 0) {
    arm64_emit_word(e, arm64_movz(1, ARM64_X0, (uint16_t)fixed_status, 0));
  }
  arm64_emit_word(e, arm64_movz(1, ARM64_X8, SYS_EXIT, 0));
  arm64_emit_word(e, ARM64_SVC0);
}

static void emit_runtime_stub(Arm64Emit *e, int id, int malloc_label) {
  switch (id) {
  case STUB_MALLOC: emit_stub_malloc(e); break;
  case STUB_CALLOC: emit_stub_calloc(e, malloc_label); break;
  case STUB_FREE: emit_stub_free(e); break;
  case STUB_PUTS: emit_str_print(e, 1, 1); break;
  case STUB_PUTCHAR: emit_stub_putchar(e); break;
  case STUB_WRITE: emit_stub_write(e); break;
  case STUB_STRLEN: emit_stub_strlen(e); break;
  case STUB_MEMCPY: emit_stub_memcpy(e); break;
  case STUB_MEMMOVE: emit_stub_memmove(e); break;
  case STUB_MEMSET: emit_stub_memset(e); break;
  case STUB_EXIT: emit_stub_exit(e, -1); break;
  case STUB_ABORT: emit_stub_exit(e, 134); break;
  case STUB_STR_FROM_INT: emit_string_from_value(e, malloc_label, 1); break;
  case STUB_STR_FROM_UINT: emit_string_from_value(e, malloc_label, 0); break;
  case STUB_STR_FROM_BOOL: emit_string_from_bool(e, malloc_label); break;
  case STUB_STR_FROM_CHAR: emit_string_from_char(e, malloc_label); break;
  case STUB_STR_EQ: emit_string_eq(e); break;
  case STUB_STR_FREE: emit_stub_free(e); break;
  /* memory_region and memory_region_end mark a region for the safety
     runtime, which a self-contained ELF does not carry: they do nothing. */
  case STUB_REGION_BEGIN: emit_stub_free(e); break;
  case STUB_REGION_END: emit_stub_free(e); break;
  default: arm64_fail(e, "internal: no runtime stub %d", id); break;
  }
}

static void data_free(Arm64Data *d) {
  free(d->bytes);
  free(d->names);
  free(d->offs);
  free(d->fixup_fns);
  free(d->fixup_offs);
  memset(d, 0, sizeof(*d));
}

static size_t data_reserve(Arm64Data *d, size_t n, size_t align) {
  if (align < 1) align = 1;
  size_t off = (d->len + align - 1) & ~(align - 1);
  size_t need = off + n;
  if (need > d->cap) {
    size_t cap = d->cap ? d->cap : 256;
    while (cap < need) cap *= 2;
    unsigned char *grown = realloc(d->bytes, cap);
    if (!grown) return (size_t)-1;
    d->bytes = grown;
    d->cap = cap;
  }
  memset(d->bytes + d->len, 0, need - d->len);
  d->len = need;
  return off;
}

static int data_add_fn_fixup(Arm64Data *d, const char *fn, size_t off) {
  if (d->fixup_count == d->fixup_cap) {
    int cap = d->fixup_cap ? d->fixup_cap * 2 : 8;
    const char **f = realloc(d->fixup_fns, (size_t)cap * sizeof(*f));
    size_t *o = realloc(d->fixup_offs, (size_t)cap * sizeof(*o));
    if (f) d->fixup_fns = f;
    if (o) d->fixup_offs = o;
    if (!f || !o) return 0;
    d->fixup_cap = cap;
  }
  d->fixup_fns[d->fixup_count] = fn;
  d->fixup_offs[d->fixup_count] = off;
  d->fixup_count++;
  return 1;
}

static int data_name(Arm64Data *d, const char *name, size_t off) {
  if (d->count == d->name_cap) {
    int cap = d->name_cap ? d->name_cap * 2 : 16;
    const char **n = realloc(d->names, (size_t)cap * sizeof(*n));
    size_t *o = realloc(d->offs, (size_t)cap * sizeof(*o));
    if (n) d->names = n;
    if (o) d->offs = o;
    if (!n || !o) return 0;
    d->name_cap = cap;
  }
  d->names[d->count] = name;
  d->offs[d->count] = off;
  d->count++;
  return 1;
}

static int build_data_image(Arm64Data *d, const IRProgram *prog, Arm64Emit *e) {
  if (data_reserve(d, DATA_RESERVED, 8) == (size_t)-1) {
    arm64_fail(e, "out of memory building the data image");
    return 0;
  }
  for (size_t i = 0; i < prog->module_symbol_count; i++) {
    const IRModuleSymbol *s = &prog->module_symbols[i];
    if (s->kind != IR_MODSYM_VARIABLE || s->is_extern) continue;
    if (!s->type) {
      arm64_fail(e, "global '%s' has no type", s->name);
      return 0;
    }
    if (s->has_unfoldable_initializer) {
      arm64_fail(e,
                 "global '%s' needs its initializer evaluated at startup, "
                 "which the self-contained AArch64 executable cannot do",
                 s->name);
      return 0;
    }
    size_t size = s->type->kind == MTLC_TYPE_STRING ? 16
                                                    : mtlc_type_size(s->type);
    size_t align = s->type->alignment ? s->type->alignment : 8;
    if (size == 0) {
      arm64_fail(e, "global '%s' has zero size", s->name);
      return 0;
    }
    size_t off = data_reserve(d, size, align);
    if (off == (size_t)-1 ||
        !data_name(d, module_link_name(s), off) ||
        (module_link_name(s) != s->name && !data_name(d, s->name, off))) {
      arm64_fail(e, "out of memory reserving global '%s'", s->name);
      return 0;
    }
  }

  for (size_t i = 0; i < prog->module_symbol_count; i++) {
    const IRModuleSymbol *s = &prog->module_symbols[i];
    if (s->kind != IR_MODSYM_VARIABLE || s->is_extern) continue;
    long base = data_offset_of(d, module_link_name(s));
    if (base < 0) continue;
    unsigned char *slot = d->bytes + base;

    if (s->type->kind == MTLC_TYPE_STRING) {
      if (!s->has_initializer || !s->init_string) continue;
      size_t length = s->init_string_length;
      size_t chars = data_reserve(d, length + 1, 1);
      if (chars == (size_t)-1) {
        arm64_fail(e, "out of memory storing the text of global '%s'", s->name);
        return 0;
      }
      memcpy(d->bytes + chars, s->init_string, length + 1);
      slot = d->bytes + base;
      uint64_t address = (uint64_t)ELF_DATA_VADDR + (uint64_t)chars;
      uint64_t encoded_length = (uint64_t)length;
      memcpy(slot, &address, 8);
      memcpy(slot + 8, &encoded_length, 8);
      continue;
    }

    if (s->type->kind == MTLC_TYPE_STRUCT || s->type->kind == MTLC_TYPE_ARRAY) {
      if (!s->init_bytes || s->init_bytes_size == 0) continue;
      size_t size = mtlc_type_size(s->type);
      size_t n = s->init_bytes_size < size ? s->init_bytes_size : size;
      memcpy(slot, s->init_bytes, n);
      for (size_t r = 0; r < s->init_reloc_count; r++) {
        const IRInitReloc *reloc = &s->init_relocs[r];
        uint64_t address = 0;
        if (reloc->symbol) {
          long target = data_offset_of(d, reloc->symbol);
          if (target < 0) {
            arm64_fail(e,
                       "global '%s' points at '%s', which the self-contained "
                       "AArch64 executable cannot address",
                       s->name, reloc->symbol);
            return 0;
          }
          address = (uint64_t)ELF_DATA_VADDR + (uint64_t)target;
        } else {
          size_t length = reloc->string ? reloc->string_length : 0;
          size_t chars = data_reserve(d, length + 1, 1);
          if (chars == (size_t)-1) {
            arm64_fail(e, "out of memory storing a string in global '%s'",
                       s->name);
            return 0;
          }
          memcpy(d->bytes + chars, reloc->string ? reloc->string : "",
                 length + 1);
          address = (uint64_t)ELF_DATA_VADDR + (uint64_t)chars;
          if (reloc->string_wants_record) {
            size_t record = data_reserve(d, 16, 8);
            if (record == (size_t)-1) {
              arm64_fail(e, "out of memory storing a string record in '%s'",
                         s->name);
              return 0;
            }
            uint64_t encoded_length = (uint64_t)length;
            memcpy(d->bytes + record, &address, 8);
            memcpy(d->bytes + record + 8, &encoded_length, 8);
            address = (uint64_t)ELF_DATA_VADDR + (uint64_t)record;
          }
        }
        if (reloc->offset + 8 > size) continue;
        memcpy(d->bytes + base + reloc->offset, &address, 8);
      }
      continue;
    }

    if (s->type->kind == MTLC_TYPE_POINTER && s->has_initializer &&
        s->init_string) {
      size_t length = s->init_string_length;
      size_t chars = data_reserve(d, length + 1, 1);
      if (chars == (size_t)-1) {
        arm64_fail(e, "out of memory storing the text of global '%s'", s->name);
        return 0;
      }
      memcpy(d->bytes + chars, s->init_string, length + 1);
      uint64_t address = (uint64_t)ELF_DATA_VADDR + (uint64_t)chars;
      memcpy(d->bytes + base, &address, 8);
      continue;
    }

    if (s->init_symbol_ref ||
        (s->init_reloc_count > 0 && s->init_relocs[0].symbol)) {
      const char *target =
          s->init_symbol_ref ? s->init_symbol_ref : s->init_relocs[0].symbol;
      long toff = data_offset_of(d, target);
      if (toff >= 0) {
        uint64_t address = (uint64_t)ELF_DATA_VADDR + (uint64_t)toff;
        memcpy(slot, &address, 8);
      } else if (prog && ir_program_lookup_symbol(prog, target) &&
                 ir_program_lookup_symbol(prog, target)->kind ==
                     IR_MODSYM_FUNCTION) {
        if (!data_add_fn_fixup(d, target, (size_t)base)) {
          arm64_fail(e, "out of memory noting '%s' points at code", s->name);
          return 0;
        }
      } else {
        arm64_fail(e, "global '%s' points at '%s', which the self-contained "
                      "AArch64 executable cannot address",
                   s->name, target);
        return 0;
      }
      continue;
    }
    if (!s->has_initializer) continue;
    size_t size = mtlc_type_size(s->type);
    if (size == 0 || size > 8) {
      arm64_fail(e, "global '%s' has an unsupported scalar size %u", s->name,
                 (unsigned)size);
      return 0;
    }
    unsigned char bytes[8] = {0};
    if (s->init_is_float) {
      double value = 0.0;
      memcpy(&value, &s->init_bits, 8);
      if (s->type->kind == MTLC_TYPE_FLOAT32) {
        float narrowed = (float)value;
        memcpy(bytes, &narrowed, 4);
      } else {
        memcpy(bytes, &value, 8);
      }
    } else {
      uint64_t bits = (uint64_t)s->init_bits;
      memcpy(bytes, &bits, size);
    }
    memcpy(slot, bytes, size);
  }
  return 1;
}

static int find_fn(const IRProgram *prog, const char *name) {
  for (size_t i = 0; i < prog->function_count; i++) {
    if (strcmp(prog->functions[i]->name, name) == 0) {
      return (int)i;
    }
  }
  return -1;
}

int arm64_ir_encode_program(Arm64Emit *e, const IRProgram *prog,
                            const char *entry, unsigned char **data_out,
                            size_t *data_len_out) {
  LblMap fns = {0};
  Arm64Data data = {0};
  e->code_vaddr = (uint64_t)ELF_BASE + ELF_HDRS;
  if (data_out) *data_out = NULL;
  if (data_len_out) *data_len_out = 0;
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
    arm64_fail(e, "out of memory walking the call graph from '%s'", entry);
    return 0;
  }
  if (!build_data_image(&data, prog, e)) {
    free(reach);
    free(queue);
    free(retf);
    data_free(&data);
    return 0;
  }

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

  int qh = 0, qt = 0;
  int start = find_fn(prog, entry);
  if (start < 0) {
    free(reach);
    free(queue);
    free(retf);
    free(fns.names);
    free(fns.ids);
    data_free(&data);
    arm64_fail(e, "no entry function '%s' in this program", entry);
    return 0;
  }
  reach[start] = 1;
  queue[qt++] = start;
  while (qh < qt) {
    int fi = queue[qh++];
    const IRFunction *f = prog->functions[fi];
    if (io_leaf(f->name)) {
      continue;
    }
    for (size_t k = 0; k < f->instruction_count; k++) {
      const IRInstruction *in = &f->instructions[k];
      const char *referenced[5] = {
          in->op == IR_OP_CALL ? in->text : NULL,
          in->dest.kind == IR_OPERAND_SYMBOL ? in->dest.name : NULL,
          in->lhs.kind == IR_OPERAND_SYMBOL ? in->lhs.name : NULL,
          in->rhs.kind == IR_OPERAND_SYMBOL ? in->rhs.name : NULL, NULL};
      for (int o = 0; o < 4; o++) {
        if (!referenced[o]) continue;
        int ci = find_fn(prog, referenced[o]);
        if (ci >= 0 && !reach[ci]) {
          reach[ci] = 1;
          queue[qt++] = ci;
        }
      }
      for (size_t a = 0; a < in->argument_count; a++) {
        if (in->arguments[a].kind != IR_OPERAND_SYMBOL) continue;
        int ci = find_fn(prog, in->arguments[a].name);
        if (ci >= 0 && !reach[ci]) {
          reach[ci] = 1;
          queue[qt++] = ci;
        }
      }
    }
  }

  int stub_used[STUB_COUNT] = {0};
  int cstr_needs_body = 0;
  for (size_t i = 0; i < n; i++) {
    if (!reach[i]) continue;
    const IRFunction *f = prog->functions[i];
    if (io_leaf(f->name)) continue;
    for (size_t k = 0; k < f->instruction_count; k++) {
      const IRInstruction *in = &f->instructions[k];
      if (in->op == IR_OP_NEW) {
        stub_used[STUB_MALLOC] = 1;
      } else if (binary_is_string_concat(in)) {
        stub_used[STUB_MALLOC] = 1;
        stub_used[STUB_MEMCPY] = 1;
      } else if (in->op == IR_OP_CALL && in->text &&
                 (strcmp(in->text, "mettle_crash_trap") == 0 ||
                  strcmp(in->text, "mettle_crash_trap_ex") == 0)) {
        stub_used[STUB_PUTS] = 1;
      } else if (in->op == IR_OP_CALL && in->text &&
                 find_fn(prog, in->text) < 0) {
        int id = call_stub_index(prog, in->text);
        if (id >= 0) stub_used[id] = 1;
      } else if (in->op == IR_OP_CALL && in->text &&
                 strcmp(in->text, "cstr") == 0 &&
                 (in->argument_count == 0 ||
                  in->arguments[0].kind != IR_OPERAND_STRING)) {
        cstr_needs_body = 1;
      }
    }
  }
  for (int i = 0; i < STUB_COUNT; i++) {
    if (stub_used[i] && RUNTIME_STUBS[i].needs_malloc) stub_used[STUB_MALLOC] = 1;
  }

  for (int i = 0; i < data.fixup_count; i++) {
    arm64_emit_label_address(e, ARM64_X9,
                             label_for(e, &fns, data.fixup_fns[i]));
    emit_imm(e, ARM64_X10,
             (uint64_t)ELF_DATA_VADDR + (uint64_t)data.fixup_offs[i]);
    arm64_emit_word(e, arm64_str_imm(1, ARM64_X9, ARM64_X10, 0));
    int fi = find_fn(prog, data.fixup_fns[i]);
    if (fi >= 0 && !reach[fi]) {
      reach[fi] = 1;
      queue[qt++] = fi;
    }
  }
  while (qh < qt) {
    int fi = queue[qh++];
    const IRFunction *f = prog->functions[fi];
    for (size_t k = 0; k < f->instruction_count; k++) {
      const IRInstruction *in = &f->instructions[k];
      if (in->op == IR_OP_CALL && in->text) {
        int ci = find_fn(prog, in->text);
        if (ci >= 0 && !reach[ci]) {
          reach[ci] = 1;
          queue[qt++] = ci;
        }
      }
    }
  }
  arm64_emit_word(e, arm64_ldr_imm(1, ARM64_X0, ARM64_SP, 0));
  arm64_emit_word(e, arm64_add_imm(1, ARM64_X1, ARM64_SP, 8, 0));
  arm64_emit_bl(e, label_for(e, &fns, entry));
  arm64_emit_word(e, arm64_movz(1, ARM64_X8, SYS_EXIT, 0));
  arm64_emit_word(e, ARM64_SVC0);

  for (size_t i = 0; i < n && !e->error; i++) {
    if (!reach[i]) {
      continue;
    }
    const IRFunction *fn = prog->functions[i];
    if (strcmp(fn->name, "cstr") == 0 && !cstr_needs_body) {
      continue;
    }
    arm64_bind_label(e, label_for(e, &fns, fn->name));
    int with_newline = 0;
    if (io_stub_intrinsic(fn->name, &with_newline, NULL)) {
      emit_str_print(e, with_newline, 0);
    } else if (!encode_function(e, fn, &fns, prog, retf, NULL, &data)) {
      break;
    }
  }

  int malloc_label = stub_used[STUB_MALLOC]
                         ? label_for(e, &fns, RUNTIME_STUBS[STUB_MALLOC].name)
                         : -1;
  for (int i = 0; i < STUB_COUNT && !e->error; i++) {
    if (!stub_used[i]) continue;
    arm64_bind_label(e, label_for(e, &fns, RUNTIME_STUBS[i].name));
    emit_runtime_stub(e, i, malloc_label);
  }

  free(reach);
  free(queue);
  free(retf);
  free(fns.names);
  free(fns.ids);
  if (e->error) {
    data_free(&data);
    return 0;
  }
  if (data_out && data_len_out) {
    *data_out = data.bytes;
    *data_len_out = data.len;
    data.bytes = NULL;
  }
  data_free(&data);
  return 1;
}

static void arm64_object_error(char *error, size_t capacity,
                               const char *message) {
  if (error && capacity > 0) {
    snprintf(error, capacity, "%s", message ? message : "unknown error");
  }
}

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
      size_t length = reloc->string ? reloc->string_length : 0;
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
      size_t length = symbol->init_string_length;
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

  if (symbol->type->kind == MTLC_TYPE_POINTER && symbol->has_initializer &&
      symbol->init_string) {
    char chars_symbol[64];
    size_t chars_offset = 0;
    size_t slot = 0;
    size_t length = symbol->init_string_length;
    snprintf(chars_symbol, sizeof(chars_symbol), ".Lmtlc.gstr.%u",
             object->string_id++);
    if (!binary_emitter_append_bytes(emitter, object->rodata_section,
                                     symbol->init_string, length + 1,
                                     &chars_offset) ||
        !binary_emitter_define_symbol(emitter, chars_symbol,
                                      BINARY_SYMBOL_LOCAL,
                                      object->rodata_section, chars_offset,
                                      length + 1) ||
        !binary_emitter_align_section(emitter, data_section, 8, 0) ||
        !binary_emitter_append_zeros(emitter, data_section, 8, &slot) ||
        !binary_emitter_add_relocation(emitter, data_section, slot,
                                       BINARY_RELOCATION_ADDR64, chars_symbol,
                                       0)) {
      return 0;
    }
    return binary_emitter_define_symbol(emitter, link_name,
                                        BINARY_SYMBOL_GLOBAL, data_section,
                                        slot, 8);
  }

  if (symbol->init_symbol_ref && symbol->init_symbol_ref[0]) {
    const char *target = symbol->init_symbol_ref;
    const IRModuleSymbol *referenced =
        object->program ? ir_program_lookup_symbol(object->program, target)
                        : NULL;
    size_t slot = 0;
    if (symbol->type->kind != MTLC_TYPE_POINTER &&
        symbol->type->kind != MTLC_TYPE_FUNCTION_POINTER) {
      return 0;
    }
    if (referenced) {
      target = module_link_name(referenced);
    }
    if (!binary_emitter_align_section(emitter, data_section, 8, 0) ||
        !binary_emitter_append_zeros(emitter, data_section, 8, &slot) ||
        !binary_emitter_add_relocation(emitter, data_section, slot,
                                       BINARY_RELOCATION_ADDR64, target, 0)) {
      return 0;
    }
    return binary_emitter_define_symbol(emitter, link_name,
                                        BINARY_SYMBOL_GLOBAL, data_section,
                                        slot, 8);
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

static void arm64_check_callees_have_bodies(Arm64Emit *code, const LblMap *functions) {
  for (int i = 0; !code->error && i < functions->count; i++) {
    if (!code->label_bound[functions->ids[i]]) {
      arm64_fail(code, "a call targets '%s', which this object has no body for",
                 functions->names[i]);
    }
  }
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
  emitter = binary_emitter_create(
      mtlc_target()->format == BINARY_TARGET_FORMAT_MACHO_ARM64
          ? BINARY_TARGET_FORMAT_MACHO_ARM64
          : BINARY_TARGET_FORMAT_ELF_ARM64);
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
        (symbol && (symbol->is_extern || !symbol->has_body))) {
      continue;
    }
    size_t start = arm64_here(&code);
    arm64_bind_label(&code,
                     label_for(&code, &functions, function->name));
    if (!encode_function(&code, function, &functions, prog, returns_float,
                         &object, NULL)) {
      break;
    }
    const char *link_name = symbol ? module_link_name(symbol) : function->name;
    if (!binary_emitter_define_symbol(
            emitter, link_name, BINARY_SYMBOL_GLOBAL, object.text_section,
            start, arm64_here(&code) - start)) {
      arm64_fail(&code, "could not define the symbol for '%s'", link_name);
      break;
    }
  }
  arm64_check_callees_have_bodies(&code, &functions);
  if (code.error || !arm64_emit_finalize(&code)) {
    arm64_object_error(error, error_capacity,
                       code.reason[0] ? code.reason
                       : binary_emitter_get_error(emitter)
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

static void w16(unsigned char *p, uint16_t v) { memcpy(p, &v, 2); }
static void w32(unsigned char *p, uint32_t v) { memcpy(p, &v, 4); }
static void w64(unsigned char *p, uint64_t v) { memcpy(p, &v, 8); }

int arm64_write_elf(const char *path, const unsigned char *code, size_t len,
                    const unsigned char *data, size_t data_len) {
  unsigned char h[ELF_HDRS];
  memset(h, 0, sizeof(h));
  uint64_t text_total = ELF_HDRS + len;
  if (text_total >= ELF_TEXT_LIMIT) {
    return 0;
  }
  uint64_t data_off = ((text_total + 0xFFFu) & ~(uint64_t)0xFFF) + 0x1000u;
  uint64_t data_size = data_len ? data_len : 8;

  h[0] = 0x7F; h[1] = 'E'; h[2] = 'L'; h[3] = 'F';
  h[4] = 2; h[5] = 1; h[6] = 1;
  w16(h + 16, 2); w16(h + 18, 183); w32(h + 20, 1);
  w64(h + 24, ELF_BASE + ELF_HDRS); w64(h + 32, 64); w64(h + 40, 0);
  w32(h + 48, 0); w16(h + 52, 64); w16(h + 54, 56); w16(h + 56, 2);
  unsigned char *ph = h + 64;
  w32(ph + 0, 1); w32(ph + 4, 5); w64(ph + 8, 0);
  w64(ph + 16, ELF_BASE); w64(ph + 24, ELF_BASE);
  w64(ph + 32, text_total); w64(ph + 40, text_total); w64(ph + 48, 0x1000);
  ph += 56;
  w32(ph + 0, 1); w32(ph + 4, 6); w64(ph + 8, data_off);
  w64(ph + 16, ELF_DATA_VADDR); w64(ph + 24, ELF_DATA_VADDR);
  w64(ph + 32, data_size); w64(ph + 40, data_size); w64(ph + 48, 0x1000);

  FILE *f = fopen(path, "wb");
  if (!f) {
    return 0;
  }
  static const unsigned char zeros[512] = {0};
  int ok = fwrite(h, 1, ELF_HDRS, f) == ELF_HDRS &&
           fwrite(code, 1, len, f) == len;
  for (uint64_t at = text_total; ok && at < data_off;) {
    size_t chunk = (size_t)(data_off - at);
    if (chunk > sizeof(zeros)) chunk = sizeof(zeros);
    ok = fwrite(zeros, 1, chunk, f) == chunk;
    at += chunk;
  }
  if (ok && data_len) {
    ok = fwrite(data, 1, data_len, f) == data_len;
  } else if (ok) {
    ok = fwrite(zeros, 1, 8, f) == 8;
  }
  fclose(f);
  return ok;
}
