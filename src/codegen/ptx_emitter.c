#include "ptx_emitter.h"
#include "ir/ir_device_report.h"
#include "ir/ir_explain_device.h"
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef enum { PC_NONE, PC_PRED, PC_B16, PC_B32, PC_B64, PC_F32, PC_F64 } PtxClass;

typedef struct {
  PtxClass cls;
  int idx;
  int is_unsigned;
  int is_ptr;
  MtlcTypeKind elem;
  MtlcAddressSpace address_space;
  int mem_local;
  int mem_aggregate;
  int mem_addr;
  size_t mem_size;
  size_t mem_align;
  size_t ptr_align;
} PtxVal;

typedef struct {
  char *name;
  PtxVal val;
} PtxBinding;

typedef struct {
  const char *name;
  int count;
} PtxDefCount;

typedef struct {
  uint32_t id;
  IRTensorResidencyScope scope;
  int resident;
  int tuple_peak;
  PtxClass accumulator_class;
  int accumulator_base;
  int accumulator_count;
  const IRInstruction *consumed_epilogue;
} PtxTensorResidency;

typedef struct {
  char *name;
  const MtlcType *type;
  PtxClass cls;
  int base;
  int count;
  int rows;
  int cols;
  MtlcViewLayout layout;
  size_t first;
  size_t last;
  size_t line;
  int scalarized;
  int temporary;
  char current[24];
} PtxTile;

typedef struct {
  char *data;
  size_t len, cap;
} Sb;

typedef struct {
  Sb body;
  Sb declarations;
  int count[8];
  PtxBinding *binds;
  size_t nbinds, capbinds;
  PtxDefCount *def_counts;
  size_t def_capacity;
  PtxClass *asm_out_cls;
  int *asm_out_idx;
  size_t asm_out_count, asm_out_capacity;
  char async_guard[32];
  size_t async_guard_end;
  size_t async_guard_jump;
  PtxDefCount *generic_names;
  size_t generic_capacity;
  PtxTensorResidency *tensor_residencies;
  size_t tensor_residency_count, tensor_residency_capacity;
  PtxTile *tiles;
  size_t tile_count, tile_capacity;
  int tile_lane, tile_group, tile_thread, tile_pair;
  int tile_peak, tile_budget;
  size_t tile_peak_line;
  unsigned char *tile_role;
  size_t *tile_window_start;
  size_t tile_role_count;
  IRProgram *program;
  IRFunction *function;
  const IRModuleSymbol *function_symbol;
  PtxVal return_desc;
  size_t call_count;
  int target_arch;
  char target_variant;
  int isa_major;
  int isa_minor;
  int tensor_tuple_budget;
  int rounded_arithmetic;
  int reported;
  size_t *line_marks;
  size_t line_mark_count, line_mark_capacity;
  char *error;
} PtxFn;

static void sb_ensure(Sb *sb, size_t extra) {
  if (sb->len + extra + 1 <= sb->cap) {
    return;
  }
  size_t ncap = sb->cap ? sb->cap * 2 : 1024;
  while (ncap < sb->len + extra + 1) {
    ncap *= 2;
  }
  sb->data = realloc(sb->data, ncap);
  sb->cap = ncap;
}
static void sb_puts(Sb *sb, const char *s) {
  size_t n = strlen(s);
  sb_ensure(sb, n);
  memcpy(sb->data + sb->len, s, n);
  sb->len += n;
  sb->data[sb->len] = 0;
}
static void sb_printf(Sb *sb, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  va_list ap2;
  va_copy(ap2, ap);
  int need = vsnprintf(NULL, 0, fmt, ap2);
  va_end(ap2);
  if (need < 0) {
    va_end(ap);
    return;
  }
  sb_ensure(sb, (size_t)need);
  vsnprintf(sb->data + sb->len, (size_t)need + 1, fmt, ap);
  sb->len += (size_t)need;
  va_end(ap);
}

static void fn_error(PtxFn *fn, const char *fmt, ...) {
  if (fn->error) {
    return;
  }
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  fn->error = strdup(buf);
}

static const char *cls_prefix(PtxClass c) {
  switch (c) {
  case PC_PRED:
    return "%p";
  case PC_B16:
    return "%rs";
  case PC_B32:
    return "%r";
  case PC_B64:
    return "%rd";
  case PC_F32:
    return "%f";
  case PC_F64:
    return "%fd";
  default:
    return "%?";
  }
}
static const char *cls_regtype(PtxClass c) {
  switch (c) {
  case PC_PRED:
    return ".pred";
  case PC_B16:
    return ".b16";
  case PC_B32:
    return ".b32";
  case PC_B64:
    return ".b64";
  case PC_F32:
    return ".f32";
  case PC_F64:
    return ".f64";
  default:
    return ".b32";
  }
}
static int new_reg(PtxFn *fn, PtxClass c) { return fn->count[c]++; }
static PtxClass elem_class(MtlcTypeKind elem, int *is_unsigned);
static const char *mem_type_suffix(MtlcTypeKind elem);
static int ptx_type_is_aggregate(const MtlcType *type);
static PtxVal operand_desc(PtxFn *fn, const IROperand *op);
static void use_as(PtxFn *fn, const IROperand *op, PtxClass want, char *out);
static int ptx_branch_skips_only_async_copies(const IRFunction *func,
                                              size_t at, size_t *end,
                                              size_t *jump);
static void coerce(PtxFn *fn, PtxClass scls, int s_unsigned, const char *srcreg,
                   PtxClass want, char *out);
static void reg_name(PtxClass c, int idx, char *out) {
  snprintf(out, 24, "%s%d", cls_prefix(c), idx);
}

static PtxBinding *find_binding(PtxFn *fn, const char *name) {
  for (size_t i = 0; i < fn->nbinds; i++) {
    if (strcmp(fn->binds[i].name, name) == 0) {
      return &fn->binds[i];
    }
  }
  return NULL;
}
static PtxBinding *named_binding(PtxFn *fn, const IROperand *op) {
  if (!op || !op->name ||
      (op->kind != IR_OPERAND_TEMP && op->kind != IR_OPERAND_SYMBOL)) {
    return NULL;
  }
  return find_binding(fn, op->name);
}

static size_t ptx_name_hash(const char *s) {
  size_t h = (size_t)1469598103934665603ull;
  for (; *s; s++) {
    h ^= (unsigned char)*s;
    h *= (size_t)1099511628211ull;
  }
  return h;
}

static PtxDefCount *ptx_def_slot(PtxFn *fn, const char *name) {
  if (!fn->def_capacity || !name) {
    return NULL;
  }
  size_t mask = fn->def_capacity - 1;
  size_t i = ptx_name_hash(name) & mask;
  while (fn->def_counts[i].name && strcmp(fn->def_counts[i].name, name) != 0) {
    i = (i + 1) & mask;
  }
  return &fn->def_counts[i];
}

static int ptx_name_is_redefined(PtxFn *fn, const char *name) {
  PtxDefCount *slot = ptx_def_slot(fn, name);
  return slot && slot->name && slot->count > 1;
}

static PtxVal *ptx_append_binding(PtxFn *fn, const char *name, PtxVal v) {
  if (fn->nbinds == fn->capbinds) {
    fn->capbinds = fn->capbinds ? fn->capbinds * 2 : 16;
    fn->binds = realloc(fn->binds, fn->capbinds * sizeof(PtxBinding));
  }
  fn->binds[fn->nbinds].name = strdup(name);
  fn->binds[fn->nbinds].val = v;
  return &fn->binds[fn->nbinds++].val;
}

static int ptx_register_is_bound(PtxFn *fn, PtxClass cls, int idx) {
  for (size_t i = 0; i < fn->nbinds; i++) {
    const PtxVal *val = &fn->binds[i].val;
    if (val->mem_local || val->mem_aggregate) {
      if (cls == PC_B64 && val->mem_addr == idx) {
        return 1;
      }
      continue;
    }
    if (val->cls == cls && val->idx == idx) {
      return 1;
    }
  }
  return 0;
}

static PtxVal *ptx_bind_into_home(PtxFn *fn, PtxBinding *b, const char *name,
                                  PtxVal v) {
  PtxVal home = v;
  if (b) {
    home.cls = b->val.cls;
    home.idx = b->val.idx;
  } else if (!ptx_register_is_bound(fn, v.cls, v.idx)) {
    return ptx_append_binding(fn, name, v);
  } else {
    home.idx = new_reg(fn, v.cls);
  }
  char src[24], use[24], dst[24];
  reg_name(v.cls, v.idx, src);
  coerce(fn, v.cls, v.is_unsigned, src, home.cls, use);
  reg_name(home.cls, home.idx, dst);
  if (strcmp(dst, use) != 0) {
    sb_printf(&fn->body, "\tmov%s %s, %s;\n", cls_regtype(home.cls), dst, use);
  }
  if (b) {
    b->val = home;
    return &b->val;
  }
  return ptx_append_binding(fn, name, home);
}

static PtxVal *bind_value(PtxFn *fn, const char *name, PtxVal v) {
  PtxBinding *b = find_binding(fn, name);
  if (!v.mem_local && !v.mem_aggregate && v.cls != PC_NONE &&
      (!b || (!b->val.mem_local && !b->val.mem_aggregate &&
              b->val.cls != PC_NONE)) &&
      ptx_name_is_redefined(fn, name)) {
    return ptx_bind_into_home(fn, b, name, v);
  }
  if (b) {
    if (b->val.mem_local && !v.mem_local) {
      if (!b->val.mem_aggregate) {
        int u = 0;
        PtxClass want = elem_class(b->val.elem, &u);
        char src[24], addr[24], use[24];
        reg_name(v.cls, v.idx, src);
        reg_name(PC_B64, b->val.mem_addr, addr);
        coerce(fn, v.cls, v.is_unsigned, src, want, use);
        sb_printf(&fn->body, "\tst.local.%s [%s], %s;\n",
                  mem_type_suffix(b->val.elem), addr, use);
      }
      return &b->val;
    }
    b->val = v;
    return &b->val;
  }
  return ptx_append_binding(fn, name, v);
}

static PtxVal destination_value(PtxFn *fn, const IROperand *dest,
                                PtxVal computed) {
  if (dest && dest->kind == IR_OPERAND_SYMBOL && dest->name) {
    PtxBinding *home = find_binding(fn, dest->name);
    if (home) {
      if (home->val.mem_local) {
        computed.idx = new_reg(fn, computed.cls);
        return computed;
      }
      if (home->val.cls != computed.cls) {
        fn_error(fn,
                 "PTX: direct write to symbol '%s' changes register class %d -> %d",
                 dest->name, (int)home->val.cls, (int)computed.cls);
        return computed;
      }
      return home->val;
    }
  }
  computed.idx = new_reg(fn, computed.cls);
  return computed;
}

static size_t ptx_offset_alignment(const IRInstruction *in, const PtxVal *la,
                                   const PtxVal *ra) {
  const PtxVal *pointer = la->is_ptr ? la : ra;
  const IROperand *offset = la->is_ptr ? &in->rhs : &in->lhs;
  const char *op = in->text ? in->text : "+";
  if (!pointer->ptr_align || offset->kind != IR_OPERAND_INT ||
      (strcmp(op, "+") != 0 && strcmp(op, "-") != 0)) {
    return 0;
  }
  unsigned long long bits =
      (unsigned long long)pointer->ptr_align | (unsigned long long)offset->int_value;
  return (size_t)(bits & (~bits + 1ULL));
}

static long long ptx_copy_alignment(const PtxVal *dest, const PtxVal *source) {
  size_t a = dest->ptr_align ? dest->ptr_align : 4;
  size_t b = source->ptr_align ? source->ptr_align : 4;
  size_t align = a < b ? a : b;
  return align >= 8 ? 8 : (long long)align;
}

static MtlcTypeKind base_kind_from_name(const char *s, int *is_unsigned) {
  *is_unsigned = (strstr(s, "uint") != NULL || strstr(s, "bool") != NULL);
  if (strstr(s, "float16")) {
    return MTLC_TYPE_FLOAT16;
  }
  if (strstr(s, "bfloat16") || strstr(s, "bf16")) {
    return MTLC_TYPE_BFLOAT16;
  }
  if (strstr(s, "float32") || strstr(s, "f32")) {
    return MTLC_TYPE_FLOAT32;
  }
  if (strstr(s, "float64") || strstr(s, "double") || strstr(s, "float")) {
    return MTLC_TYPE_FLOAT64;
  }
  if (strstr(s, "int64") || strstr(s, "uint64")) {
    return *is_unsigned ? MTLC_TYPE_UINT64 : MTLC_TYPE_INT64;
  }
  if (strstr(s, "int16") || strstr(s, "uint16")) {
    return *is_unsigned ? MTLC_TYPE_UINT16 : MTLC_TYPE_INT16;
  }
  if (strstr(s, "int8") || strstr(s, "uint8")) {
    return *is_unsigned ? MTLC_TYPE_UINT8 : MTLC_TYPE_INT8;
  }
  if (strstr(s, "int32") || strstr(s, "uint32")) {
    return *is_unsigned ? MTLC_TYPE_UINT32 : MTLC_TYPE_INT32;
  }
  if (strstr(s, "bool")) {
    return MTLC_TYPE_BOOL;
  }
  return MTLC_TYPE_INT32;
}
static PtxClass class_of_kind(MtlcTypeKind k, int *is_unsigned) {
  switch (k) {
  case MTLC_TYPE_INT8:
  case MTLC_TYPE_INT16:
  case MTLC_TYPE_INT32:
    *is_unsigned = 0;
    return PC_B32;
  case MTLC_TYPE_UINT8:
  case MTLC_TYPE_UINT16:
  case MTLC_TYPE_UINT32:
  case MTLC_TYPE_BOOL:
    *is_unsigned = 1;
    return PC_B32;
  case MTLC_TYPE_INT64:
    *is_unsigned = 0;
    return PC_B64;
  case MTLC_TYPE_UINT64:
    *is_unsigned = 1;
    return PC_B64;
  case MTLC_TYPE_FLOAT32:
    *is_unsigned = 0;
    return PC_F32;
  case MTLC_TYPE_FLOAT64:
    *is_unsigned = 0;
    return PC_F64;
  case MTLC_TYPE_FLOAT16:
  case MTLC_TYPE_BFLOAT16:
    *is_unsigned = 0;
    return PC_F32;
  case MTLC_TYPE_POINTER:
  case MTLC_TYPE_ARRAY:
  case MTLC_TYPE_STRING:
  case MTLC_TYPE_FUNCTION_POINTER:
    *is_unsigned = 1;
    return PC_B64;
  default:
    *is_unsigned = 0;
    return PC_B32;
  }
}
static PtxVal descriptor_from_typename(const char *name) {
  PtxVal v = {0};
  if (!name) {
    name = "int64";
  }
  int ptr = (strchr(name, '*') != NULL) || strstr(name, "cstring") ||
            strstr(name, "string");
  int isu = 0;
  MtlcTypeKind base = base_kind_from_name(name, &isu);
  if (strstr(name, "cstring")) {
    base = MTLC_TYPE_UINT8;
  }
  if (ptr) {
    v.cls = PC_B64;
    v.is_ptr = 1;
    v.is_unsigned = 1;
    const char *firstStar = strchr(name, '*');
    v.elem = (firstStar && strchr(firstStar + 1, '*')) ? MTLC_TYPE_POINTER : base;
    v.address_space = MTLC_ADDRESS_SPACE_GENERIC;
  } else {
    int u = 0;
    v.cls = class_of_kind(base, &u);
    v.is_unsigned = u;
    v.elem = base;
  }
  return v;
}

static PtxVal descriptor_from_type(const MtlcType *type) {
  PtxVal v = {0};
  if (!type) {
    return descriptor_from_typename(NULL);
  }
  if (type->kind == MTLC_TYPE_POINTER) {
    v.cls = PC_B64;
    v.is_ptr = 1;
    v.is_unsigned = 1;
    v.elem = type->base_type ? type->base_type->kind : MTLC_TYPE_VOID;
    if (ptx_type_is_aggregate(type->base_type)) {
      v.elem = MTLC_TYPE_VOID;
    }
    v.address_space = type->address_space == MTLC_ADDRESS_SPACE_DEFAULT
                          ? MTLC_ADDRESS_SPACE_GLOBAL
                          : type->address_space;
  } else {
    int u = 0;
    v.cls = class_of_kind(type->kind, &u);
    v.is_unsigned = u;
    v.elem = type->kind;
  }
  return v;
}

static int ptx_type_is_aggregate(const MtlcType *type) {
  return type && (type->kind == MTLC_TYPE_STRUCT ||
                  type->kind == MTLC_TYPE_ARRAY ||
                  type->kind == MTLC_TYPE_TAGGED_ENUM);
}

static size_t ptx_class_width(PtxClass cls) {
  switch (cls) {
  case PC_B16: return 2;
  case PC_B64:
  case PC_F64: return 8;
  case PC_PRED:
  case PC_B32:
  case PC_F32: return 4;
  case PC_NONE: return 0;
  }
  return 0;
}

static void ptx_block_copy(PtxFn *fn, const char *dst_space,
                           const char *dst_base, const char *src_space,
                           const char *src_base, size_t size,
                           size_t alignment) {
  size_t widest = alignment ? alignment : 1;
  if (widest > 8) {
    widest = 8;
  }
  for (size_t offset = 0; offset < size;) {
    size_t chunk = widest;
    while (chunk > 1 && (chunk > size - offset || offset % chunk != 0)) {
      chunk /= 2;
    }
    PtxClass cls = (chunk == 8) ? PC_B64 : PC_B32;
    const char *type = (chunk == 8)   ? "b64"
                       : (chunk == 4) ? "b32"
                       : (chunk == 2) ? "u16"
                                      : "u8";
    char reg[24];
    reg_name(cls, new_reg(fn, cls), reg);
    sb_printf(&fn->body, "\tld%s.%s %s, [%s+%zu];\n", src_space, type, reg,
              src_base, offset);
    sb_printf(&fn->body, "\tst%s.%s [%s+%zu], %s;\n", dst_space, type,
              dst_base, offset, reg);
    offset += chunk;
  }
}

static int ptx_space_is_plain(const MtlcType *type, const char *type_name) {
  if (type) {
    return type->kind == MTLC_TYPE_POINTER &&
           type->address_space == MTLC_ADDRESS_SPACE_DEFAULT;
  }
  if (!type_name || !strchr(type_name, '*')) {
    return 0;
  }
  return !strstr(type_name, "global") && !strstr(type_name, "shared") &&
         !strstr(type_name, "constant") && !strstr(type_name, "local") &&
         !strstr(type_name, "generic");
}

static int ptx_space_needs_generic(const MtlcType *type) {
  return type && type->kind == MTLC_TYPE_POINTER &&
         type->address_space != MTLC_ADDRESS_SPACE_DEFAULT &&
         type->address_space != MTLC_ADDRESS_SPACE_GLOBAL &&
         type->address_space != MTLC_ADDRESS_SPACE_CONSTANT;
}

static PtxDefCount *ptx_generic_slot(PtxFn *fn, const char *name) {
  if (!fn->generic_capacity || !name) {
    return NULL;
  }
  size_t mask = fn->generic_capacity - 1;
  size_t i = ptx_name_hash(name) & mask;
  while (fn->generic_names[i].name &&
         strcmp(fn->generic_names[i].name, name) != 0) {
    i = (i + 1) & mask;
  }
  return &fn->generic_names[i];
}

static int ptx_name_may_be_generic(PtxFn *fn, const char *name) {
  PtxDefCount *slot = ptx_generic_slot(fn, name);
  return slot && slot->name != NULL;
}

static int ptx_mark_generic(PtxFn *fn, const char *name) {
  PtxDefCount *slot = ptx_generic_slot(fn, name);
  if (!slot || slot->name) {
    return 0;
  }
  slot->name = name;
  slot->count = 1;
  return 1;
}

static int ptx_operand_may_be_generic(PtxFn *fn, const IROperand *op) {
  return op && op->name &&
         (op->kind == IR_OPERAND_TEMP || op->kind == IR_OPERAND_SYMBOL) &&
         ptx_name_may_be_generic(fn, op->name);
}

static void ptx_collect_generic_names(PtxFn *fn, const IRFunction *func,
                                      const IRModuleSymbol *symbol) {
  size_t needed = (func->instruction_count + func->parameter_count + 1) * 2;
  size_t capacity = 16;
  while (capacity < needed) {
    capacity *= 2;
  }
  fn->generic_names = calloc(capacity, sizeof(PtxDefCount));
  if (!fn->generic_names) {
    return;
  }
  fn->generic_capacity = capacity;
  if (!func->is_kernel) {
    for (size_t p = 0; p < func->parameter_count; p++) {
      const MtlcType *pt = symbol && symbol->param_types &&
                                   p < symbol->param_count
                               ? symbol->param_types[p]
                               : NULL;
      const char *tn =
          func->parameter_types ? func->parameter_types[p] : NULL;
      if (func->parameter_names && func->parameter_names[p] &&
          ptx_space_is_plain(pt, tn)) {
        ptx_mark_generic(fn, func->parameter_names[p]);
      }
    }
  }
  for (size_t i = 0; i < func->instruction_count; i++) {
    const IRInstruction *in = &func->instructions[i];
    if (!in->dest.name || (in->dest.kind != IR_OPERAND_TEMP &&
                           in->dest.kind != IR_OPERAND_SYMBOL)) {
      continue;
    }
    if (in->op == IR_OP_ADDRESS_SPACE_ALLOC || in->op == IR_OP_ADDRESS_OF ||
        ((in->op == IR_OP_DECLARE_LOCAL ||
          ir_instruction_writes_destination(in)) &&
         ptx_space_needs_generic(in->value_type))) {
      ptx_mark_generic(fn, in->dest.name);
    }
  }
  int changed = 1;
  while (changed) {
    changed = 0;
    for (size_t i = 0; i < func->instruction_count; i++) {
      const IRInstruction *in = &func->instructions[i];
      if (!in->dest.name || (in->dest.kind != IR_OPERAND_TEMP &&
                             in->dest.kind != IR_OPERAND_SYMBOL) ||
          ptx_name_may_be_generic(fn, in->dest.name)) {
        continue;
      }
      int flows = 0;
      switch (in->op) {
      case IR_OP_ASSIGN:
      case IR_OP_CAST:
        flows = ptx_operand_may_be_generic(fn, &in->lhs);
        break;
      case IR_OP_BINARY:
        flows = ptx_operand_may_be_generic(fn, &in->lhs) ||
                ptx_operand_may_be_generic(fn, &in->rhs);
        break;
      case IR_OP_SELECT:
        flows = ptx_operand_may_be_generic(fn, &in->lhs) ||
                ptx_operand_may_be_generic(fn, &in->rhs);
        for (size_t a = 0; a < in->argument_count && !flows; a++) {
          flows = ptx_operand_may_be_generic(fn, &in->arguments[a]);
        }
        break;
      default:
        break;
      }
      if (flows && ptx_mark_generic(fn, in->dest.name)) {
        changed = 1;
      }
    }
  }
}

static const char *ptx_space_conversion(MtlcAddressSpace from,
                                        MtlcAddressSpace to) {
  if (to == MTLC_ADDRESS_SPACE_GENERIC) {
    if (from == MTLC_ADDRESS_SPACE_WORKGROUP) return "cvta.shared.u64";
    if (from == MTLC_ADDRESS_SPACE_PRIVATE) return "cvta.local.u64";
    return NULL;
  }
  if (from == MTLC_ADDRESS_SPACE_GENERIC) {
    if (to == MTLC_ADDRESS_SPACE_WORKGROUP) return "cvta.to.shared.u64";
    if (to == MTLC_ADDRESS_SPACE_PRIVATE) return "cvta.to.local.u64";
  }
  return NULL;
}

static int ptx_convert_space_into(PtxFn *fn, MtlcAddressSpace from,
                                  MtlcAddressSpace to, const char *dst,
                                  const char *src) {
  const char *op = ptx_space_conversion(from, to);
  if (!op) {
    return 0;
  }
  sb_printf(&fn->body, "\t%s %s, %s;\n", op, dst, src);
  return 1;
}

static void ptx_convert_space(PtxFn *fn, MtlcAddressSpace from,
                              MtlcAddressSpace to, char *reg) {
  if (!ptx_space_conversion(from, to)) {
    return;
  }
  char converted[24];
  reg_name(PC_B64, new_reg(fn, PC_B64), converted);
  ptx_convert_space_into(fn, from, to, converted, reg);
  snprintf(reg, 24, "%s", converted);
}

static void ptx_generic_address(PtxFn *fn, const IROperand *op, char *reg) {
  PtxVal v = operand_desc(fn, op);
  if (!v.is_ptr || v.address_space != MTLC_ADDRESS_SPACE_PRIVATE) {
    return;
  }
  char converted[24];
  reg_name(PC_B64, new_reg(fn, PC_B64), converted);
  sb_printf(&fn->body, "\tcvta.local.u64 %s, %s;\n", converted, reg);
  snprintf(reg, 24, "%s", converted);
}

static int ptx_local_address_taken(const IRFunction *func, const char *name) {
  if (!func || !name) {
    return 0;
  }
  for (size_t i = 0; i < func->instruction_count; i++) {
    const IRInstruction *in = &func->instructions[i];
    if (in->op == IR_OP_ADDRESS_OF && in->lhs.kind == IR_OPERAND_SYMBOL &&
        in->lhs.name && strcmp(in->lhs.name, name) == 0) {
      return 1;
    }
  }
  return 0;
}

static const char *ptx_memory_space(MtlcAddressSpace address_space) {
  switch (address_space) {
  case MTLC_ADDRESS_SPACE_GLOBAL: return ".global";
  case MTLC_ADDRESS_SPACE_WORKGROUP: return ".shared";
  case MTLC_ADDRESS_SPACE_CONSTANT: return ".global";
  case MTLC_ADDRESS_SPACE_PRIVATE: return ".local";
  case MTLC_ADDRESS_SPACE_DEFAULT:
  case MTLC_ADDRESS_SPACE_GENERIC:
    return "";
  }
  return NULL;
}

typedef struct {
  unsigned char width;
  unsigned char absorbed;
  size_t members[4];
} PtxVectorPlan;

static int ptx_fold_constant(PtxFn *fn, const IRFunction *func, size_t before,
                             const IROperand *operand, long long *out,
                             unsigned depth) {
  if (!func || !operand || depth > 8) return 0;
  if (operand->kind == IR_OPERAND_INT) {
    *out = operand->int_value;
    return 1;
  }
  if ((operand->kind != IR_OPERAND_TEMP && operand->kind != IR_OPERAND_SYMBOL) ||
      !operand->name || ptx_name_is_redefined(fn, operand->name))
    return 0;
  for (size_t i = before; i > 0; i--) {
    const IRInstruction *producer = &func->instructions[i - 1];
    if (!producer->dest.name || !producer->dest.kind ||
        strcmp(producer->dest.name, operand->name) != 0)
      continue;
    if (producer->op == IR_OP_ASSIGN || producer->op == IR_OP_CAST)
      return ptx_fold_constant(fn, func, i - 1, &producer->lhs, out, depth + 1);
    if (producer->op == IR_OP_BINARY && producer->text) {
      long long lhs = 0, rhs = 0;
      if (!ptx_fold_constant(fn, func, i - 1, &producer->lhs, &lhs, depth + 1) ||
          !ptx_fold_constant(fn, func, i - 1, &producer->rhs, &rhs, depth + 1))
        return 0;
      if (strcmp(producer->text, "*") == 0) { *out = lhs * rhs; return 1; }
      if (strcmp(producer->text, "+") == 0) { *out = lhs + rhs; return 1; }
      if (strcmp(producer->text, "-") == 0) { *out = lhs - rhs; return 1; }
      if (strcmp(producer->text, "<<") == 0 && rhs >= 0 && rhs < 32) {
        *out = lhs << rhs;
        return 1;
      }
      return 0;
    }
    return 0;
  }
  return 0;
}

static int ptx_operand_is_stable(PtxFn *fn, const IROperand *operand) {
  if (!operand) return 0;
  if (operand->kind != IR_OPERAND_TEMP && operand->kind != IR_OPERAND_SYMBOL)
    return 1;
  return operand->name && !ptx_name_is_redefined(fn, operand->name);
}

static int ptx_ends_straight_line(const IRInstruction *in) {
  return in->op == IR_OP_LABEL || in->op == IR_OP_JUMP ||
         in->op == IR_OP_BRANCH_ZERO || in->op == IR_OP_BRANCH_EQ ||
         in->op == IR_OP_RETURN || in->op == IR_OP_CALL ||
         in->op == IR_OP_CALL_INDIRECT || in->op == IR_OP_INLINE_ASM ||
         in->op == IR_OP_PHI;
}

static int ptx_name_settled(const IRFunction *func, const char *name,
                            size_t from, size_t to) {
  for (size_t k = from; k < to; k++) {
    const IRInstruction *in = &func->instructions[k];
    if (ptx_ends_straight_line(in)) return 0;
    if (in->dest.name && strcmp(in->dest.name, name) == 0) return 0;
  }
  return 1;
}

static int ptx_redefines(const IRInstruction *in, const IROperand *operand) {
  return operand && operand->name && in->dest.name &&
         strcmp(in->dest.name, operand->name) == 0;
}

static int ptx_operand_names(const IROperand *operand, const char *name) {
  return operand->name && strcmp(operand->name, name) == 0;
}

static int ptx_touches_name(const IRInstruction *in, const char *name) {
  if (ptx_operand_names(&in->lhs, name) || ptx_operand_names(&in->rhs, name) ||
      (in->dest.name && strcmp(in->dest.name, name) == 0))
    return 1;
  for (size_t a = 0; a < in->argument_count; a++) {
    if (ptx_operand_names(&in->arguments[a], name)) return 1;
  }
  return 0;
}

static int ptx_member_can_move(const IRFunction *func, size_t head,
                               size_t member) {
  const char *name = func->instructions[member].dest.name;
  if (!name) return 1;
  for (size_t k = head + 1; k < member; k++) {
    if (ptx_touches_name(&func->instructions[k], name)) return 0;
  }
  return 1;
}

static void ptx_address_parts(PtxFn *fn, const IRFunction *func, size_t index,
                              const IROperand *address,
                              const IROperand **base, long long *offset) {
  *base = address;
  *offset = 0;
  if (!func || !address || !address->name ||
      (address->kind != IR_OPERAND_TEMP && address->kind != IR_OPERAND_SYMBOL))
    return;
  int stable = ptx_operand_is_stable(fn, address);
  for (size_t i = index; i > 0; i--) {
    const IRInstruction *producer = &func->instructions[i - 1];
    long long constant = 0;
    if (!producer->dest.name ||
        strcmp(producer->dest.name, address->name) != 0) {
      if (!stable && ptx_ends_straight_line(producer)) {
        *base = NULL;
        return;
      }
      continue;
    }
    if (producer->op != IR_OP_BINARY || !producer->text ||
        strcmp(producer->text, "+") != 0)
      return;
    if ((ptx_operand_is_stable(fn, &producer->lhs) ||
         (producer->lhs.name &&
          ptx_name_settled(func, producer->lhs.name, i, index))) &&
        ptx_fold_constant(fn, func, i - 1, &producer->rhs, &constant, 0)) {
      *base = &producer->lhs;
      *offset = constant;
    }
    return;
  }
  if (!stable) *base = NULL;
}

static int ptx_vector_gap_is_safe(const IRInstruction *in) {
  switch (in->op) {
  case IR_OP_NOP:
  case IR_OP_ASSIGN:
  case IR_OP_BINARY:
  case IR_OP_UNARY:
  case IR_OP_CAST:
  case IR_OP_LOAD:
  case IR_OP_SELECT:
  case IR_OP_ADDRESS_OF:
    return 1;
  default:
    return 0;
  }
}

static int ptx_vector_element_ok(MtlcTypeKind elem) {
  return elem == MTLC_TYPE_FLOAT32 || elem == MTLC_TYPE_INT32 ||
         elem == MTLC_TYPE_UINT32 || elem == MTLC_TYPE_FLOAT64 ||
         elem == MTLC_TYPE_INT64 || elem == MTLC_TYPE_UINT64;
}

static const MtlcType *ptx_load_pointer_type(const IRProgram *program,
                                             const IRFunction *func,
                                             const IROperand *base,
                                             size_t index) {
  const IRModuleSymbol *symbol = NULL;
  if (!func || !base || !base->name) return NULL;
  for (size_t i = index < func->instruction_count ? index : func->instruction_count;
       i > 0; i--) {
    const IRInstruction *producer = &func->instructions[i - 1];
    if (ptx_ends_straight_line(producer)) break;
    if (!producer->dest.name || strcmp(producer->dest.name, base->name) != 0)
      continue;
    if (producer->op == IR_OP_CAST && producer->value_type &&
        producer->value_type->kind == MTLC_TYPE_POINTER &&
        producer->value_type->pointee_align)
      return producer->value_type;
    break;
  }
  symbol = program ? ir_program_lookup_symbol(program, func->name) : NULL;
  if (base->kind == IR_OPERAND_SYMBOL && symbol &&
      symbol->kind == IR_MODSYM_FUNCTION) {
    for (size_t p = 0; p < func->parameter_count && p < symbol->param_count;
         p++) {
      if (func->parameter_names && func->parameter_names[p] &&
          strcmp(func->parameter_names[p], base->name) == 0)
        return symbol->param_types ? symbol->param_types[p] : NULL;
    }
  }
  for (size_t i = func->instruction_count; i > 0; i--) {
    const IRInstruction *producer = &func->instructions[i - 1];
    if (!producer->dest.name || strcmp(producer->dest.name, base->name) != 0)
      continue;
    if (producer->op == IR_OP_LOAD || producer->op == IR_OP_ASSIGN ||
        producer->op == IR_OP_CAST || producer->op == IR_OP_BINARY ||
        producer->op == IR_OP_ADDRESS_SPACE_ALLOC)
      return producer->value_type;
    break;
  }
  if (program && base->kind == IR_OPERAND_SYMBOL) {
    const IRModuleSymbol *global = ir_program_lookup_symbol(program, base->name);
    if (global && global->kind == IR_MODSYM_VARIABLE) return global->type;
  }
  return NULL;
}

static void ptx_report_vector_subject(const IRFunction *func, size_t head,
                                      const IROperand *base, const char *what,
                                      size_t count, char *subject,
                                      size_t size) {
  const char *root = ir_device_report_address_root(func, head, base);
  char shown[128];
  if (root) {
    ir_display_name(root, shown, sizeof(shown));
    snprintf(subject, size, "%zu %s from `%s`", count, what, shown);
  } else {
    snprintf(subject, size, "%zu %s", count, what);
  }
}

static void ptx_report_vector_decline(const IRFunction *func,
                                      size_t head, const IROperand *base,
                                      const MtlcType *pointer_type,
                                      size_t stride, size_t found) {
  size_t width = found >= 4 ? 4 : 2;
  char subject[200], reason[240], fix[200];
  ptx_report_vector_subject(func, head, base, "adjacent loads", found, subject,
                            sizeof(subject));
  if (pointer_type->pointee_align) {
    snprintf(reason, sizeof(reason),
             "the pointer's type declares %zu byte alignment, and one load of "
             "%zu elements needs %zu",
             pointer_type->pointee_align, width, stride * width);
  } else {
    snprintf(reason, sizeof(reason),
             "the pointer's type declares no alignment, and one load of %zu "
             "elements needs %zu bytes",
             width, stride * width);
  }
  if (base && base->kind == IR_OPERAND_SYMBOL) {
    snprintf(fix, sizeof(fix), "declare `align(%zu)` in the pointer's type",
             stride * width);
  } else {
    snprintf(fix, sizeof(fix),
             "cast the address the loads start from to a pointer type with "
             "`align(%zu)`; the cast is accepted where the offset is proven a "
             "multiple of %zu",
             stride * width, stride * width);
  }
  ir_explain_device_note_site(func->name, func->instructions[head].location.line,
                              "vector", subject, IR_DEVICE_MISS,
                              "kept as separate loads", reason, fix,
                              "vector-load-alignment");
}

static size_t ptx_vector_members(PtxFn *fn, const IRFunction *func,
                                 const PtxVectorPlan *plan, size_t head,
                                 const IROperand *base, long long offset,
                                 size_t stride, size_t *members) {
  size_t found = 1;
  members[0] = head;
  for (size_t j = head + 1; j < func->instruction_count && found < 4; j++) {
    const IRInstruction *next = &func->instructions[j];
    const IROperand *next_base = NULL;
    long long next_offset = 0;
    if (next->op == IR_OP_LOAD && !plan[j].absorbed) {
      ptx_address_parts(fn, func, j, &next->lhs, &next_base, &next_offset);
      if (next_base && base && next_base->name && base->name &&
          next_base->kind == base->kind &&
          strcmp(next_base->name, base->name) == 0 &&
          next_offset == offset + (long long)(stride * found)) {
        if (!ptx_member_can_move(func, head, j)) break;
        members[found++] = j;
        if (ptx_redefines(next, base)) break;
        continue;
      }
    }
    if (!ptx_vector_gap_is_safe(next)) break;
    if (ptx_redefines(next, base)) break;
  }
  return found;
}

static void ptx_vector_declined(const IRFunction *func, size_t head,
                                const IROperand *base,
                                const MtlcType *pointer_type, size_t stride,
                                size_t found, const size_t *members,
                                unsigned char *declined) {
  if (!declined || found < 2) return;
  ptx_report_vector_decline(func, head, base, pointer_type, stride, found);
  for (size_t k = 1; k < found; k++) declined[members[k]] = 1;
}

static PtxVectorPlan *ptx_plan_vector_loads(PtxFn *fn,
                                            const IRProgram *program,
                                            const IRFunction *func) {
  PtxVectorPlan *plan;
  unsigned char *declined = NULL;
  if (!func || func->instruction_count == 0) return NULL;
  plan = calloc(func->instruction_count, sizeof(*plan));
  if (!plan) return NULL;
  if (fn->reported) declined = calloc(func->instruction_count, 1);
  for (size_t i = 0; i < func->instruction_count; i++) {
    const IRInstruction *head = &func->instructions[i];
    const IROperand *base = NULL;
    const MtlcType *pointer_type = NULL;
    long long offset = 0;
    size_t stride = 0;
    size_t members[4];
    size_t found = 1;
    if (head->op != IR_OP_LOAD || plan[i].absorbed) continue;
    if (declined && declined[i]) continue;
    ptx_address_parts(fn, func, i, &head->lhs, &base, &offset);
    pointer_type = ptx_load_pointer_type(program, func, base, i);
    if (!pointer_type || pointer_type->kind != MTLC_TYPE_POINTER ||
        !pointer_type->base_type ||
        (!pointer_type->pointee_align && !declined) ||
        !ptx_vector_element_ok(pointer_type->base_type->kind))
      continue;
    if (pointer_type->address_space != MTLC_ADDRESS_SPACE_GLOBAL &&
        pointer_type->address_space != MTLC_ADDRESS_SPACE_WORKGROUP &&
        pointer_type->address_space != MTLC_ADDRESS_SPACE_CONSTANT)
      continue;
    stride = pointer_type->base_type->size;
    if (!stride || (size_t)offset % (stride * 2) != 0) continue;
    found = ptx_vector_members(fn, func, plan, i, base, offset, stride, members);
    if (found >= 4 && pointer_type->pointee_align >= stride * 4 &&
        (size_t)offset % (stride * 4) == 0) {
      plan[i].width = 4;
    } else if (found >= 2 && pointer_type->pointee_align >= stride * 2) {
      plan[i].width = 2;
    } else {
      ptx_vector_declined(func, i, base, pointer_type, stride, found, members,
                          declined);
      continue;
    }
    for (size_t k = 1; k < plan[i].width; k++) {
      plan[members[k]].absorbed = 1;
      plan[members[k]].width = 0;
    }
    for (size_t k = 0; k < plan[i].width; k++) {
      plan[i].members[k] = members[k];
    }
  }
  free(declined);
  return plan;
}

static void ptx_emit_uniform_check(PtxFn *fn, const IRInstruction *in,
                                   PtxVal value, int target_arch);

static const char *ptx_load_space(MtlcAddressSpace address_space) {
  if (address_space == MTLC_ADDRESS_SPACE_CONSTANT) {
    return ".global.nc";
  }
  return ptx_memory_space(address_space);
}

static PtxClass elem_class(MtlcTypeKind elem, int *is_unsigned) {
  return class_of_kind(elem, is_unsigned);
}

static const char *mem_type_suffix(MtlcTypeKind elem) {
  switch (elem) {
  case MTLC_TYPE_INT8:
    return "s8";
  case MTLC_TYPE_UINT8:
    return "u8";
  case MTLC_TYPE_INT16:
    return "s16";
  case MTLC_TYPE_UINT16:
    return "u16";
  case MTLC_TYPE_INT32:
    return "s32";
  case MTLC_TYPE_UINT32:
  case MTLC_TYPE_BOOL:
    return "u32";
  case MTLC_TYPE_INT64:
    return "s64";
  case MTLC_TYPE_UINT64:
  case MTLC_TYPE_POINTER:
  case MTLC_TYPE_ARRAY:
  case MTLC_TYPE_STRING:
  case MTLC_TYPE_FUNCTION_POINTER:
    return "u64";
  case MTLC_TYPE_FLOAT32:
    return "f32";
  case MTLC_TYPE_FLOAT64:
    return "f64";
  case MTLC_TYPE_FLOAT16:
  case MTLC_TYPE_BFLOAT16:
    return "b16";
  default:
    return "u32";
  }
}

static void coerce(PtxFn *fn, PtxClass scls, int s_unsigned, const char *srcreg,
                   PtxClass want, char *out) {
  if (scls == want) {
    snprintf(out, 24, "%s", srcreg);
    return;
  }
  int idx = new_reg(fn, want);
  reg_name(want, idx, out);
  if (scls == PC_B32 && want == PC_B64) {
    sb_printf(&fn->body, "\tcvt.%s.%s %s, %s;\n", s_unsigned ? "u64" : "s64",
              s_unsigned ? "u32" : "s32", out, srcreg);
  } else if (scls == PC_B64 && want == PC_B32) {
    sb_printf(&fn->body, "\tcvt.u32.u64 %s, %s;\n", out, srcreg);
  } else if (scls == PC_B32 && want == PC_F32) {
    sb_printf(&fn->body, "\tcvt.rn.f32.%s %s, %s;\n", s_unsigned ? "u32" : "s32",
              out, srcreg);
  } else if (scls == PC_B32 && want == PC_F64) {
    sb_printf(&fn->body, "\tcvt.rn.f64.%s %s, %s;\n", s_unsigned ? "u32" : "s32",
              out, srcreg);
  } else if (scls == PC_B64 && want == PC_F32) {
    sb_printf(&fn->body, "\tcvt.rn.f32.%s %s, %s;\n", s_unsigned ? "u64" : "s64",
              out, srcreg);
  } else if (scls == PC_B64 && want == PC_F64) {
    sb_printf(&fn->body, "\tcvt.rn.f64.%s %s, %s;\n", s_unsigned ? "u64" : "s64",
              out, srcreg);
  } else if (scls == PC_F32 && want == PC_B32) {
    sb_printf(&fn->body, "\tcvt.rzi.s32.f32 %s, %s;\n", out, srcreg);
  } else if (scls == PC_F64 && want == PC_B32) {
    sb_printf(&fn->body, "\tcvt.rzi.s32.f64 %s, %s;\n", out, srcreg);
  } else if (scls == PC_F32 && want == PC_B64) {
    sb_printf(&fn->body, "\tcvt.rzi.s64.f32 %s, %s;\n", out, srcreg);
  } else if (scls == PC_F64 && want == PC_B64) {
    sb_printf(&fn->body, "\tcvt.rzi.s64.f64 %s, %s;\n", out, srcreg);
  } else if (scls == PC_F32 && want == PC_F64) {
    sb_printf(&fn->body, "\tcvt.f64.f32 %s, %s;\n", out, srcreg);
  } else if (scls == PC_F64 && want == PC_F32) {
    sb_printf(&fn->body, "\tcvt.rn.f32.f64 %s, %s;\n", out, srcreg);
  } else if (scls == PC_PRED && want == PC_B32) {
    sb_printf(&fn->body, "\tselp.u32 %s, 1, 0, %s;\n", out, srcreg);
  } else if (scls == PC_B32 && want == PC_PRED) {
    sb_printf(&fn->body, "\tsetp.ne.u32 %s, %s, 0;\n", out, srcreg);
  } else {
    fn_error(fn, "unsupported PTX coercion class %d -> %d", scls, want);
  }
}

static uint32_t f32_bits(double v) {
  float f = (float)v;
  uint32_t b;
  memcpy(&b, &f, 4);
  return b;
}
static uint64_t f64_bits(double v) {
  uint64_t b;
  memcpy(&b, &v, 8);
  return b;
}

static void use_as(PtxFn *fn, const IROperand *op, PtxClass want, char *out) {
  if (op->kind == IR_OPERAND_INT) {
    int idx = new_reg(fn, want);
    reg_name(want, idx, out);
    if (want == PC_B32) {
      sb_printf(&fn->body, "\tmov.u32 %s, %lld;\n", out,
                (long long)op->int_value);
    } else if (want == PC_B64) {
      sb_printf(&fn->body, "\tmov.u64 %s, %lld;\n", out,
                (long long)op->int_value);
    } else if (want == PC_F32) {
      sb_printf(&fn->body, "\tmov.f32 %s, 0f%08X;\n", out,
                f32_bits((double)op->int_value));
    } else if (want == PC_F64) {
      sb_printf(&fn->body, "\tmov.f64 %s, 0d%016llX;\n", out,
                (unsigned long long)f64_bits((double)op->int_value));
    } else if (want == PC_PRED) {
      sb_printf(&fn->body, "\tsetp.ne.u32 %s, %lld, 0;\n", out,
                (long long)op->int_value);
    }
    return;
  }
  if (op->kind == IR_OPERAND_FLOAT) {
    PtxClass fc = (op->float_bits == 32) ? PC_F32 : PC_F64;
    int idx = new_reg(fn, fc);
    char tmp[24];
    reg_name(fc, idx, tmp);
    if (fc == PC_F32) {
      sb_printf(&fn->body, "\tmov.f32 %s, 0f%08X;\n", tmp,
                f32_bits(op->float_value));
    } else {
      sb_printf(&fn->body, "\tmov.f64 %s, 0d%016llX;\n", tmp,
                (unsigned long long)f64_bits(op->float_value));
    }
    coerce(fn, fc, 0, tmp, want, out);
    return;
  }
  PtxBinding *b = (op->name) ? find_binding(fn, op->name) : NULL;
  if (!b) {
    fn_error(fn, "PTX: use of undefined value '%s'",
             op->name ? op->name : "?");
    snprintf(out, 24, "%%r0");
    return;
  }
  if (b->val.mem_local) {
    if (b->val.mem_aggregate) {
      fn_error(fn, "PTX: aggregate local '%s' has no scalar value", op->name);
      snprintf(out, 24, "%%r0");
      return;
    }
    int u = 0;
    PtxClass sc = elem_class(b->val.elem, &u);
    char tmp[24], addr[24];
    reg_name(sc, new_reg(fn, sc), tmp);
    reg_name(PC_B64, b->val.mem_addr, addr);
    sb_printf(&fn->body, "\tld.local.%s %s, [%s];\n",
              mem_type_suffix(b->val.elem), tmp, addr);
    coerce(fn, sc, b->val.is_unsigned, tmp, want, out);
    return;
  }
  char src[24];
  reg_name(b->val.cls, b->val.idx, src);
  coerce(fn, b->val.cls, b->val.is_unsigned, src, want, out);
}

static char **g_ptx_strings = NULL;
static size_t g_ptx_string_count = 0;
static size_t g_ptx_string_capacity = 0;
static int g_ptx_uses_vprintf = 0;
static int g_ptx_emit_checks = 0;
static int g_ptx_report_types = 0;
static long long g_ptx_spaced_accesses = 0;
static long long g_ptx_generic_accesses = 0;
static long long g_ptx_vector_groups = 0;
static long long g_ptx_vector_loads_saved = 0;
static double g_ptx_analysis_seconds = 0.0;

static void ptx_emit_uniform_check(PtxFn *fn, const IRInstruction *in,
                                   PtxVal value, int target_arch) {
  char self[24], leader[24], same[24], all[24], bits[24];
  if (!g_ptx_emit_checks || !in->uniform_value || target_arch < 70) {
    return;
  }
  if (value.cls != PC_B32 && value.cls != PC_F32) {
    return;
  }
  reg_name(value.cls, value.idx, self);
  if (value.cls == PC_F32) {
    reg_name(PC_B32, new_reg(fn, PC_B32), bits);
    sb_printf(&fn->body, "\tmov.b32 %s, %s;\n", bits, self);
  } else {
    snprintf(bits, sizeof(bits), "%s", self);
  }
  reg_name(PC_B32, new_reg(fn, PC_B32), leader);
  reg_name(PC_PRED, new_reg(fn, PC_PRED), same);
  reg_name(PC_PRED, new_reg(fn, PC_PRED), all);
  sb_printf(&fn->body,
            "\tshfl.sync.idx.b32 %s, %s, 0, 0x1f, 0xffffffff;\n"
            "\tsetp.eq.b32 %s, %s, %s;\n"
            "\tvote.sync.all.pred %s, %s, 0xffffffff;\n"
            "\t@!%s trap;\n",
            leader, bits, same, bits, leader, all, same, all);
}

static int ptx_string_index(const char *text) {
  if (!text) return -1;
  for (size_t i = 0; i < g_ptx_string_count; i++) {
    if (strcmp(g_ptx_strings[i], text) == 0) return (int)i;
  }
  return -1;
}

static int ptx_string_intern(const char *text) {
  int existing = ptx_string_index(text);
  if (existing >= 0) return existing;
  if (g_ptx_string_count == g_ptx_string_capacity) {
    size_t grown = g_ptx_string_capacity ? g_ptx_string_capacity * 2 : 8;
    char **resized = realloc(g_ptx_strings, grown * sizeof(*resized));
    if (!resized) return -1;
    g_ptx_strings = resized;
    g_ptx_string_capacity = grown;
  }
  g_ptx_strings[g_ptx_string_count] = strdup(text ? text : "");
  if (!g_ptx_strings[g_ptx_string_count]) return -1;
  return (int)g_ptx_string_count++;
}

static void ptx_string_pool_reset(void) {
  for (size_t i = 0; i < g_ptx_string_count; i++) free(g_ptx_strings[i]);
  free(g_ptx_strings);
  g_ptx_strings = NULL;
  g_ptx_string_count = 0;
  g_ptx_string_capacity = 0;
  g_ptx_uses_vprintf = 0;
}

static const char *ptx_literal_string(const IRFunction *function,
                                      const IROperand *operand) {
  if (!operand) return NULL;
  if (operand->kind == IR_OPERAND_STRING) return operand->name;
  if (!function || operand->kind != IR_OPERAND_TEMP || !operand->name) {
    return NULL;
  }
  for (size_t i = function->instruction_count; i > 0;) {
    i--;
    const IRInstruction *in = &function->instructions[i];
    if (in->dest.kind != IR_OPERAND_TEMP || !in->dest.name ||
        strcmp(in->dest.name, operand->name) != 0) {
      continue;
    }
    if (in->op == IR_OP_LOAD && in->lhs.kind == IR_OPERAND_STRING) {
      return in->lhs.name;
    }
    return NULL;
  }
  return NULL;
}

static int ptx_intrinsic_is_print(MtlcIntrinsic intrinsic) {
  return intrinsic == MTLC_INTRINSIC_GPU_PRINT ||
         intrinsic == MTLC_INTRINSIC_GPU_PRINT_I32 ||
         intrinsic == MTLC_INTRINSIC_GPU_PRINT_F32 ||
         intrinsic == MTLC_INTRINSIC_GPU_PRINT_2I32;
}

static PtxVal operand_desc(PtxFn *fn, const IROperand *op) {
  PtxVal v = {0};
  if (op->kind == IR_OPERAND_INT) {
    v.cls = (op->int_value > 2147483647LL || op->int_value < -2147483648LL)
                ? PC_B64
                : PC_B32;
    return v;
  }
  if (op->kind == IR_OPERAND_FLOAT) {
    v.cls = (op->float_bits == 32) ? PC_F32 : PC_F64;
    return v;
  }
  PtxBinding *b = (op->name) ? find_binding(fn, op->name) : NULL;
  if (b) {
    return b->val;
  }
  v.cls = PC_B32;
  return v;
}

static int sanitize_into(const char *s, char *out, size_t cap) {
  size_t j = 0;
  for (size_t i = 0; s[i] && j + 1 < cap; i++) {
    char c = s[i];
    out[j++] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '_' || c == '$')
                   ? c
                   : '_';
  }
  out[j] = 0;
  return (int)j;
}

static const char *sreg_for_intrinsic(MtlcIntrinsic intrinsic) {
  switch (intrinsic) {
  case MTLC_INTRINSIC_GPU_LOCAL_ID_X: return "%tid.x";
  case MTLC_INTRINSIC_GPU_LOCAL_ID_Y: return "%tid.y";
  case MTLC_INTRINSIC_GPU_LOCAL_ID_Z: return "%tid.z";
  case MTLC_INTRINSIC_GPU_LOCAL_SIZE_X: return "%ntid.x";
  case MTLC_INTRINSIC_GPU_LOCAL_SIZE_Y: return "%ntid.y";
  case MTLC_INTRINSIC_GPU_LOCAL_SIZE_Z: return "%ntid.z";
  case MTLC_INTRINSIC_GPU_GROUP_ID_X: return "%ctaid.x";
  case MTLC_INTRINSIC_GPU_GROUP_ID_Y: return "%ctaid.y";
  case MTLC_INTRINSIC_GPU_GROUP_ID_Z: return "%ctaid.z";
  case MTLC_INTRINSIC_GPU_NUM_GROUPS_X: return "%nctaid.x";
  case MTLC_INTRINSIC_GPU_NUM_GROUPS_Y: return "%nctaid.y";
  case MTLC_INTRINSIC_GPU_NUM_GROUPS_Z: return "%nctaid.z";
  case MTLC_INTRINSIC_GPU_SUBGROUP_LOCAL_ID: return "%laneid";
  default: return NULL;
  }
}

static const char *ptx_atomic_scope(MtlcMemoryScope scope) {
  switch (scope) {
  case MTLC_MEMORY_SCOPE_WORK_ITEM:
  case MTLC_MEMORY_SCOPE_SUBGROUP:
  case MTLC_MEMORY_SCOPE_WORKGROUP:
    return "cta";
  case MTLC_MEMORY_SCOPE_DEVICE: return "gpu";
  case MTLC_MEMORY_SCOPE_SYSTEM: return "sys";
  case MTLC_MEMORY_SCOPE_DEFAULT: return "gpu";
  }
  return NULL;
}

static const char *ptx_atomic_order(MtlcMemoryOrder order) {
  switch (order) {
  case MTLC_MEMORY_ORDER_DEFAULT:
  case MTLC_MEMORY_ORDER_RELAXED:
    return "relaxed";
  case MTLC_MEMORY_ORDER_ACQUIRE: return "acquire";
  case MTLC_MEMORY_ORDER_RELEASE: return "release";
  case MTLC_MEMORY_ORDER_ACQ_REL: return "acq_rel";
  case MTLC_MEMORY_ORDER_SEQ_CST: return "acquire";
  }
  return NULL;
}

static const char *ptx_atomic_space(MtlcAddressSpace address_space) {
  switch (address_space) {
  case MTLC_ADDRESS_SPACE_DEFAULT:
  case MTLC_ADDRESS_SPACE_GLOBAL:
    return ".global";
  case MTLC_ADDRESS_SPACE_GENERIC: return "";
  case MTLC_ADDRESS_SPACE_WORKGROUP: return ".shared";
  case MTLC_ADDRESS_SPACE_CONSTANT:
  case MTLC_ADDRESS_SPACE_PRIVATE:
    return NULL;
  }
  return NULL;
}

static int ptx_workgroup_barrier_contract(const IRInstruction *instruction) {
  const unsigned supported = MTLC_MEMORY_REGION_WORKGROUP |
                             MTLC_MEMORY_REGION_GLOBAL;
  return instruction &&
         (instruction->memory_scope == MTLC_MEMORY_SCOPE_WORKGROUP ||
          instruction->memory_scope == MTLC_MEMORY_SCOPE_SUBGROUP) &&
         instruction->memory_order >= MTLC_MEMORY_ORDER_ACQUIRE &&
         instruction->memory_order <= MTLC_MEMORY_ORDER_SEQ_CST &&
         instruction->memory_regions != 0 &&
         (instruction->memory_regions & ~supported) == 0;
}

static int is_compare_op(const char *t) {
  return !strcmp(t, "<") || !strcmp(t, ">") || !strcmp(t, "<=") ||
         !strcmp(t, ">=") || !strcmp(t, "==") || !strcmp(t, "!=");
}

static const char *setp_cmp(const char *t, int is_float, int is_unsigned) {
  if (!strcmp(t, "==")) return "eq";
  if (!strcmp(t, "!=")) return "ne";
  if (!strcmp(t, "<")) return is_float ? "lt" : (is_unsigned ? "lo" : "lt");
  if (!strcmp(t, ">")) return is_float ? "gt" : (is_unsigned ? "hi" : "gt");
  if (!strcmp(t, "<=")) return is_float ? "le" : (is_unsigned ? "ls" : "le");
  if (!strcmp(t, ">=")) return is_float ? "ge" : (is_unsigned ? "hs" : "ge");
  return "eq";
}

static const char *type_suffix_for_class(PtxClass c, int is_unsigned) {
  switch (c) {
  case PC_B32:
    return is_unsigned ? "u32" : "s32";
  case PC_B64:
    return is_unsigned ? "u64" : "s64";
  case PC_F32:
    return "f32";
  case PC_F64:
    return "f64";
  default:
    return "s32";
  }
}

typedef enum {
  PTX_WMMA_F16,
  PTX_WMMA_BF16,
  PTX_WMMA_TF32,
  PTX_WMMA_F64,
  PTX_WMMA_I8,
  PTX_WMMA_I4,
  PTX_WMMA_B1
} PtxWmmaKind;

typedef struct {
  PtxWmmaKind kind;
  const char *shape;
  unsigned tile_m;
  unsigned tile_n;
  unsigned tile_k;
  int m_tiles;
  int n_tiles;
  const char *a_type;
  const char *b_type;
  const char *c_type;
  const char *d_type;
  PtxClass a_class;
  PtxClass b_class;
  PtxClass c_class;
  PtxClass d_class;
  int a_registers;
  int b_registers;
  int c_registers;
  int d_registers;
  int min_arch;
  int min_ptx_major;
  int min_ptx_minor;
} PtxWmmaProfile;

typedef enum {
  PTX_MMA_FP8,
  PTX_MMA_MXF8F6F4,
  PTX_MMA_MXFP4,
  PTX_MMA_NVFP4,
  PTX_MMA_SPARSE_F16,
  PTX_MMA_SPARSE_BF16,
  PTX_MMA_S8_SCALED,
  PTX_MMA_F16
} PtxMmaKind;

typedef struct {
  PtxMmaKind kind;
  const char *shape;
  const char *a_type;
  const char *b_type;
  const char *c_type;
  const char *d_type;
  const char *scale_type;
  int scale_vectors;
  int a_bits;
  int b_bits;
  int a_registers;
  int b_registers;
  int accumulator_registers;
  int m_tiles;
  int n_tiles;
  int b_halves;
  int b_signed4;
  unsigned b_zero_point;
  int c_row_scaled;
} PtxMmaProfile;

static int ptx_version_at_least(const PtxFn *fn, int major, int minor) {
  return fn->isa_major > major ||
         (fn->isa_major == major && fn->isa_minor >= minor);
}

static int ptx_async_copy_native(const PtxFn *fn) {
  return fn && fn->target_arch >= 80 && ptx_version_at_least(fn, 7, 0);
}

static int ptx_scalar_kind_bytes(MtlcTypeKind kind) {
  switch (kind) {
  case MTLC_TYPE_INT8:
  case MTLC_TYPE_UINT8:
  case MTLC_TYPE_BOOL:
    return 1;
  case MTLC_TYPE_INT16:
  case MTLC_TYPE_UINT16:
    return 2;
  case MTLC_TYPE_INT32:
  case MTLC_TYPE_UINT32:
  case MTLC_TYPE_FLOAT32:
    return 4;
  case MTLC_TYPE_INT64:
  case MTLC_TYPE_UINT64:
  case MTLC_TYPE_FLOAT64:
    return 8;
  default:
    return 0;
  }
}

static void ptx_async_address(char *buffer, size_t capacity,
                              const char *base, size_t offset) {
  if (!buffer || capacity == 0) return;
  if (offset == 0) {
    snprintf(buffer, capacity, "%s", base ? base : "0");
  } else {
    snprintf(buffer, capacity, "%s+%llu", base ? base : "0",
             (unsigned long long)offset);
  }
}

static void ptx_emit_async_copy(PtxFn *fn, const IRInstruction *in) {
  if (!fn || !in || in->argument_count != 2 ||
      in->async_copy_element_count == 0 ||
      (in->async_copy_transaction_bytes != 4 &&
       in->async_copy_transaction_bytes != 8 &&
       in->async_copy_transaction_bytes != 16)) {
    fn_error(fn, "PTX received an invalid asynchronous-copy instruction");
    return;
  }
  PtxVal destination = operand_desc(fn, &in->arguments[0]);
  PtxVal source = operand_desc(fn, &in->arguments[1]);
  int element_bytes = ptx_scalar_kind_bytes(destination.elem);
  if (!destination.is_ptr || !source.is_ptr ||
      destination.address_space != MTLC_ADDRESS_SPACE_WORKGROUP ||
      (source.address_space != MTLC_ADDRESS_SPACE_GLOBAL &&
       source.address_space != MTLC_ADDRESS_SPACE_GENERIC) ||
      destination.elem != source.elem || element_bytes <= 0) {
    fn_error(fn,
             "PTX async copy requires matching global-to-workgroup scalar pointers");
    return;
  }
  size_t bytes = (size_t)element_bytes *
                 (size_t)in->async_copy_element_count;
  size_t transaction = in->async_copy_transaction_bytes;
  if (bytes == 0 || bytes > 65536 || bytes % transaction != 0 ||
      (in->async_copy_cache == MTLC_ASYNC_CACHE_GLOBAL &&
       transaction != 16)) {
    fn_error(fn, "PTX async copy has an invalid byte/transaction contract");
    return;
  }
  char destination_wide[24], destination_base[24], source_base[24];
  use_as(fn, &in->arguments[0], PC_B64, destination_wide);
  use_as(fn, &in->arguments[1], PC_B64, source_base);
  // Workgroup addresses fit 32 bits: the narrow form lets ptxas keep the
  // whole destination computation in one register instead of a pair, which
  // matters when copies are issued among a tile's live accumulators.
  reg_name(PC_B32, new_reg(fn, PC_B32), destination_base);
  sb_printf(&fn->body, "\tcvt.u32.u64 %s, %s;\n", destination_base,
            destination_wide);
  char guard[40] = "";
  if (fn->async_guard[0]) {
    snprintf(guard, sizeof(guard), "@%s ", fn->async_guard);
  }
  if (fn->reported) {
    char headline[160], reason[200];
    int native = ptx_async_copy_native(fn);
    if (native) {
      snprintf(headline, sizeof(headline),
               "cp.async, %llu bytes in transactions of %llu",
               (unsigned long long)bytes, (unsigned long long)transaction);
    } else {
      snprintf(headline, sizeof(headline),
               "%llu bytes copied with ordinary loads and stores",
               (unsigned long long)bytes);
      snprintf(reason, sizeof(reason),
               "cp.async needs sm_80 and PTX 7.0, and this build is sm_%d at "
               "PTX %d.%d",
               fn->target_arch, fn->isa_major, fn->isa_minor);
    }
    ir_explain_device_note_site(
        fn->function->name, in->location.line, "staging",
        in->async_copy_generated ? "promoted copy into workgroup memory"
                                 : "copy into workgroup memory",
        native ? IR_DEVICE_GOOD : IR_DEVICE_FACT, headline,
        native ? NULL : reason, NULL, "async-copy");
  }
  if (ptx_async_copy_native(fn)) {
    const char *cache =
        in->async_copy_cache == MTLC_ASYNC_CACHE_GLOBAL ? "cg" : "ca";
    sb_printf(&fn->body,
              "\t// mtlc.async_copy %snative bytes=%llu transaction=%llu\n",
              in->async_copy_generated ? "auto-promoted " : "",
              (unsigned long long)bytes, (unsigned long long)transaction);
    for (size_t offset = 0; offset < bytes; offset += transaction) {
      char destination_address[48], source_address[48];
      ptx_async_address(destination_address, sizeof(destination_address),
                        destination_base, offset);
      ptx_async_address(source_address, sizeof(source_address), source_base,
                        offset);
      sb_printf(&fn->body,
                "\t%scp.async.%s.shared.global [%s], [%s], %llu;\n", guard,
                cache, destination_address, source_address,
                (unsigned long long)transaction);
    }
    return;
  }

  sb_printf(&fn->body,
            "\t// mtlc.async_copy %ssynchronous-fallback bytes=%llu transaction=%llu\n",
            in->async_copy_generated ? "auto-promoted " : "",
            (unsigned long long)bytes, (unsigned long long)transaction);
  for (size_t offset = 0; offset < bytes; offset += 4) {
    char destination_address[48], source_address[48], value[24];
    ptx_async_address(destination_address, sizeof(destination_address),
                      destination_base, offset);
    ptx_async_address(source_address, sizeof(source_address), source_base,
                      offset);
    reg_name(PC_B32, new_reg(fn, PC_B32), value);
    sb_printf(&fn->body, "\t%sld.global.b32 %s, [%s];\n", guard, value,
              source_address);
    sb_printf(&fn->body, "\t%sst.shared.b32 [%s], %s;\n", guard,
              destination_address, value);
  }
}

static void ptx_emit_async_commit(PtxFn *fn) {
  if (ptx_async_copy_native(fn)) {
    sb_puts(&fn->body, "\tcp.async.commit_group;\n");
  } else {
    sb_puts(&fn->body, "\t// mtlc.async_copy commit synchronous-fallback\n");
  }
}

static void ptx_emit_async_wait(PtxFn *fn, const IRInstruction *in) {
  if (!fn || !in || in->async_copy_pending_groups > 7) {
    if (fn) fn_error(fn, "PTX async-copy wait has an invalid group bound");
    return;
  }
  if (ptx_async_copy_native(fn)) {
    sb_printf(&fn->body, "\tcp.async.wait_group %u;\n",
              in->async_copy_pending_groups);
  } else {
    sb_printf(&fn->body,
              "\t// mtlc.async_copy wait pending=%u synchronous-fallback\n",
              in->async_copy_pending_groups);
  }
}

static int ptx_tensor_transfer_native_capable(
    const PtxFn *fn, const MtlcTensorTransferDesc *desc,
    int has_prepared_view) {
  size_t element_bytes = ir_tensor_transfer_element_bytes(desc->element);
  size_t tile_elements = ir_tensor_transfer_tile_elements(desc);
  if (!fn || !ir_tensor_transfer_desc_valid(desc) || !has_prepared_view ||
      fn->target_arch < 90 || !ptx_version_at_least(fn, 8, 3) ||
      element_bytes == 0 || tile_elements == 0 ||
      desc->global_stride_bytes[0] != element_bytes ||
      desc->element_stride[0] != 1 ||
      ((uint64_t)desc->tile_extent[0] * element_bytes) % 16u != 0 ||
      (tile_elements * element_bytes) % 16u != 0) {
    return 0;
  }
  for (uint8_t dimension = 0; dimension < desc->rank; dimension++) {
    uint64_t traversal =
        (uint64_t)desc->tile_extent[dimension] *
        desc->element_stride[dimension];
    if (desc->global_extent[dimension] > (UINT64_C(1) << 32) ||
        desc->element_stride[dimension] > 8 || traversal > 256) {
      return 0;
    }
    if (dimension != 0) {
      uint64_t stride = desc->global_stride_bytes[dimension];
      uint64_t previous_span = desc->global_stride_bytes[dimension - 1];
      if (stride % 16u != 0 || stride >= (UINT64_C(1) << 40) ||
          desc->global_extent[dimension - 1] >
              UINT64_MAX / previous_span ||
          stride < previous_span * desc->global_extent[dimension - 1]) {
        return 0;
      }
    }
  }
  return 1;
}

static void ptx_tensor_transfer_barrier_name(const PtxFn *fn, char *buffer,
                                             size_t capacity) {
  char function_name[256], raw[512];
  sanitize_into(fn && fn->function && fn->function->name
                    ? fn->function->name
                    : "kernel",
                function_name, sizeof(function_name));
  snprintf(raw, sizeof(raw), "%s_tensor_transfer_barrier", function_name);
  sanitize_into(raw, buffer, capacity);
}

static void ptx_emit_tensor_transfer_fallback(
    PtxFn *fn, const IRInstruction *in, const char *destination,
    const char *source, size_t coordinate_base, size_t label_id) {
  const MtlcTensorTransferDesc *desc = &IR_TENSOR_TRANSFER(in);
  size_t element_bytes = ir_tensor_transfer_element_bytes(desc->element);
  size_t tile_elements = ir_tensor_transfer_tile_elements(desc);
  int r_tid_x = new_reg(fn, PC_B32), r_tid_y = new_reg(fn, PC_B32);
  int r_tid_z = new_reg(fn, PC_B32), r_ntid_x = new_reg(fn, PC_B32);
  int r_ntid_y = new_reg(fn, PC_B32), r_ntid_z = new_reg(fn, PC_B32);
  int r_linear = new_reg(fn, PC_B32), r_threads = new_reg(fn, PC_B32);
  int r_scratch = new_reg(fn, PC_B32), r_quotient = new_reg(fn, PC_B32);
  int p_done = new_reg(fn, PC_PRED), p_in_bounds = new_reg(fn, PC_PRED);
  char tid_x[24], tid_y[24], tid_z[24], ntid_x[24], ntid_y[24], ntid_z[24];
  char linear[24], threads[24], scratch[24], quotient[24];
  char done[24], in_bounds[24];
  reg_name(PC_B32, r_tid_x, tid_x);
  reg_name(PC_B32, r_tid_y, tid_y);
  reg_name(PC_B32, r_tid_z, tid_z);
  reg_name(PC_B32, r_ntid_x, ntid_x);
  reg_name(PC_B32, r_ntid_y, ntid_y);
  reg_name(PC_B32, r_ntid_z, ntid_z);
  reg_name(PC_B32, r_linear, linear);
  reg_name(PC_B32, r_threads, threads);
  reg_name(PC_B32, r_scratch, scratch);
  reg_name(PC_B32, r_quotient, quotient);
  reg_name(PC_PRED, p_done, done);
  reg_name(PC_PRED, p_in_bounds, in_bounds);

  sb_printf(&fn->body,
            "\t// mtlc.tensor_transfer cooperative-fallback rank=%u bytes=%llu\n",
            (unsigned)desc->rank,
            (unsigned long long)(tile_elements * element_bytes));
  if (desc->direction == MTLC_TENSOR_TRANSFER_WORKGROUP_TO_GLOBAL)
    sb_puts(&fn->body, "\tbar.sync 0;\n");
  sb_printf(&fn->body,
            "\tmov.u32 %s, %%tid.x;\n"
            "\tmov.u32 %s, %%tid.y;\n"
            "\tmov.u32 %s, %%tid.z;\n"
            "\tmov.u32 %s, %%ntid.x;\n"
            "\tmov.u32 %s, %%ntid.y;\n"
            "\tmov.u32 %s, %%ntid.z;\n"
            "\tmad.lo.u32 %s, %s, %s, %s;\n"
            "\tmad.lo.u32 %s, %s, %s, %s;\n"
            "\tmul.lo.u32 %s, %s, %s;\n"
            "\tmul.lo.u32 %s, %s, %s;\n",
            tid_x, tid_y, tid_z, ntid_x, ntid_y, ntid_z, scratch, tid_z,
            ntid_y, tid_y, linear, scratch, ntid_x, tid_x, threads, ntid_x,
            ntid_y, threads, threads, ntid_z);
  sb_printf(&fn->body, "mtlc_tensor_transfer_%llu_loop:\n",
            (unsigned long long)label_id);
  sb_printf(&fn->body,
            "\tsetp.ge.u32 %s, %s, %llu;\n"
            "\t@%s bra mtlc_tensor_transfer_%llu_finish;\n"
            "\tmov.u32 %s, %s;\n"
            "\tmov.pred %s, 1;\n",
            done, linear, (unsigned long long)tile_elements, done,
            (unsigned long long)label_id, quotient, linear, in_bounds);

  int r_global_offset = new_reg(fn, PC_B64);
  char global_offset[24];
  reg_name(PC_B64, r_global_offset, global_offset);
  sb_printf(&fn->body, "\tmov.u64 %s, 0;\n", global_offset);
  for (uint8_t dimension = 0; dimension < desc->rank; dimension++) {
    int r_local = new_reg(fn, PC_B32);
    int r_local_step = new_reg(fn, PC_B32);
    int r_coordinate64 = new_reg(fn, PC_B64);
    int r_local64 = new_reg(fn, PC_B64);
    int r_part = new_reg(fn, PC_B64);
    int p_nonnegative = new_reg(fn, PC_PRED);
    int p_below = new_reg(fn, PC_PRED);
    char local[24], local_step[24], coordinate64[24], local64[24], part[24];
    char nonnegative[24], below[24], coordinate[24];
    reg_name(PC_B32, r_local, local);
    reg_name(PC_B32, r_local_step, local_step);
    reg_name(PC_B64, r_coordinate64, coordinate64);
    reg_name(PC_B64, r_local64, local64);
    reg_name(PC_B64, r_part, part);
    reg_name(PC_PRED, p_nonnegative, nonnegative);
    reg_name(PC_PRED, p_below, below);
    use_as(fn, &in->arguments[coordinate_base + dimension], PC_B32,
           coordinate);
    sb_printf(&fn->body,
              "\trem.u32 %s, %s, %u;\n"
              "\tdiv.u32 %s, %s, %u;\n"
              "\tmul.lo.u32 %s, %s, %u;\n"
              "\tcvt.s64.s32 %s, %s;\n"
              "\tcvt.u64.u32 %s, %s;\n"
              "\tadd.s64 %s, %s, %s;\n"
              "\tsetp.ge.s64 %s, %s, 0;\n"
              "\tsetp.lt.u64 %s, %s, %llu;\n"
              "\tand.pred %s, %s, %s;\n"
              "\tand.pred %s, %s, %s;\n"
              "\tmul.lo.u64 %s, %s, %llu;\n"
              "\tadd.u64 %s, %s, %s;\n",
              local, quotient, (unsigned)desc->tile_extent[dimension],
              quotient, quotient, (unsigned)desc->tile_extent[dimension],
              local_step, local, (unsigned)desc->element_stride[dimension],
              coordinate64, coordinate, local64, local_step, coordinate64,
              coordinate64, local64, nonnegative, coordinate64, below,
              coordinate64,
              (unsigned long long)desc->global_extent[dimension], in_bounds,
              in_bounds, nonnegative, in_bounds, in_bounds, below, part,
              coordinate64,
              (unsigned long long)desc->global_stride_bytes[dimension],
              global_offset, global_offset, part);
  }
  int r_tile_offset = new_reg(fn, PC_B64);
  int r_destination_address = new_reg(fn, PC_B64);
  int r_source_address = new_reg(fn, PC_B64);
  PtxClass value_class = element_bytes == 8 ? PC_B64 : PC_B32;
  int r_value = new_reg(fn, value_class);
  char tile_offset[24], destination_address[24], source_address[24], value[24];
  reg_name(PC_B64, r_tile_offset, tile_offset);
  reg_name(PC_B64, r_destination_address, destination_address);
  reg_name(PC_B64, r_source_address, source_address);
  reg_name(value_class, r_value, value);
  sb_printf(&fn->body,
            "\tcvt.u64.u32 %s, %s;\n"
            "\tmul.lo.u64 %s, %s, %llu;\n",
            tile_offset, linear, tile_offset, tile_offset,
            (unsigned long long)element_bytes);
  const char *load_space;
  const char *store_space;
  const char *load_base;
  const char *store_base;
  const char *load_offset;
  const char *store_offset;
  if (desc->direction == MTLC_TENSOR_TRANSFER_GLOBAL_TO_WORKGROUP) {
    load_space = "global";
    store_space = "shared";
    load_base = source;
    store_base = destination;
    load_offset = global_offset;
    store_offset = tile_offset;
  } else {
    load_space = "shared";
    store_space = "global";
    load_base = source;
    store_base = destination;
    load_offset = tile_offset;
    store_offset = global_offset;
  }
  sb_printf(&fn->body,
            "\tadd.u64 %s, %s, %s;\n"
            "\tadd.u64 %s, %s, %s;\n",
            source_address, load_base, load_offset, destination_address,
            store_base, store_offset);
  const char *load_suffix = element_bytes == 1   ? "u8"
                            : element_bytes == 2 ? "u16"
                            : element_bytes == 4 ? "b32"
                                                 : "b64";
  const char *store_suffix = element_bytes == 1   ? "u8"
                             : element_bytes == 2 ? "u16"
                             : element_bytes == 4 ? "b32"
                                                  : "b64";
  if (desc->direction == MTLC_TENSOR_TRANSFER_GLOBAL_TO_WORKGROUP) {
    sb_printf(&fn->body, "\tmov.%s %s, 0;\n", element_bytes == 8 ? "b64" : "b32",
              value);
    sb_printf(&fn->body, "\t@%s ld.%s.%s %s, [%s];\n", in_bounds,
              load_space, load_suffix, value, source_address);
    sb_printf(&fn->body, "\tst.%s.%s [%s], %s;\n", store_space,
              store_suffix, destination_address, value);
  } else {
    sb_printf(&fn->body, "\tld.%s.%s %s, [%s];\n", load_space,
              load_suffix, value, source_address);
    sb_printf(&fn->body, "\t@%s st.%s.%s [%s], %s;\n", in_bounds,
              store_space, store_suffix, destination_address, value);
  }
  sb_printf(&fn->body,
            "\tadd.u32 %s, %s, %s;\n"
            "\tbra mtlc_tensor_transfer_%llu_loop;\n"
            "mtlc_tensor_transfer_%llu_finish:\n"
            "\tbar.sync 0;\n",
            linear, linear, threads, (unsigned long long)label_id,
            (unsigned long long)label_id);
}

static void ptx_emit_tensor_transfer(PtxFn *fn, const IRInstruction *in) {
  if (!fn || !in || in->op != IR_OP_TENSOR_TRANSFER ||
      !ir_tensor_transfer_desc_valid(&IR_TENSOR_TRANSFER(in))) {
    if (fn) fn_error(fn, "PTX received an invalid tensor-transfer instruction");
    return;
  }
  const MtlcTensorTransferDesc *desc = &IR_TENSOR_TRANSFER(in);
  int has_view = in->tensor_transfer_has_prepared_view;
  size_t expected = ir_tensor_transfer_operand_count(desc, has_view);
  if (!expected || in->argument_count != expected) {
    fn_error(fn, "PTX tensor transfer has an invalid operand count");
    return;
  }
  PtxVal destination_desc = operand_desc(fn, &in->arguments[0]);
  PtxVal source_desc = operand_desc(fn, &in->arguments[1]);
  MtlcAddressSpace destination_space = destination_desc.address_space;
  MtlcAddressSpace source_space = source_desc.address_space;
  if (!destination_desc.is_ptr || !source_desc.is_ptr ||
      destination_desc.elem != source_desc.elem ||
      (desc->direction == MTLC_TENSOR_TRANSFER_GLOBAL_TO_WORKGROUP &&
       (destination_space != MTLC_ADDRESS_SPACE_WORKGROUP ||
        (source_space != MTLC_ADDRESS_SPACE_GLOBAL &&
         source_space != MTLC_ADDRESS_SPACE_GENERIC))) ||
      (desc->direction == MTLC_TENSOR_TRANSFER_WORKGROUP_TO_GLOBAL &&
       ((destination_space != MTLC_ADDRESS_SPACE_GLOBAL &&
         destination_space != MTLC_ADDRESS_SPACE_GENERIC) ||
        source_space != MTLC_ADDRESS_SPACE_WORKGROUP))) {
    fn_error(fn, "PTX tensor transfer has invalid pointer address spaces");
    return;
  }
  char destination[24], source[24];
  use_as(fn, &in->arguments[0], PC_B64, destination);
  use_as(fn, &in->arguments[1], PC_B64, source);
  size_t view_index = 2;
  size_t coordinate_base = 2u + (has_view ? 1u : 0u);
  size_t label_id = fn->call_count++;
  int native = ptx_tensor_transfer_native_capable(fn, desc, has_view);
  char map[24] = {0};
  int p_no_view = -1;
  char no_view[24] = {0};
  if (native) {
    PtxVal view_desc = operand_desc(fn, &in->arguments[view_index]);
    if (!view_desc.is_ptr ||
        (view_desc.address_space != MTLC_ADDRESS_SPACE_GLOBAL &&
         view_desc.address_space != MTLC_ADDRESS_SPACE_GENERIC)) {
      fn_error(fn, "PTX tensor transfer prepared view is not a global pointer");
      return;
    }
    use_as(fn, &in->arguments[view_index], PC_B64, map);
    p_no_view = new_reg(fn, PC_PRED);
    reg_name(PC_PRED, p_no_view, no_view);
    sb_printf(&fn->body, "\tsetp.eq.u64 %s, %s, 0;\n", no_view, map);
    sb_printf(&fn->body, "\t@%s bra mtlc_tensor_transfer_%llu_fallback;\n",
              no_view, (unsigned long long)label_id);

    int r_map_misalignment = new_reg(fn, PC_B64);
    int p_map_unaligned = new_reg(fn, PC_PRED);
    char map_misalignment[24], map_unaligned[24];
    reg_name(PC_B64, r_map_misalignment, map_misalignment);
    reg_name(PC_PRED, p_map_unaligned, map_unaligned);
    sb_printf(&fn->body,
              "\tand.b64 %s, %s, 63;\n"
              "\tsetp.ne.u64 %s, %s, 0;\n"
              "\t@%s bra mtlc_tensor_transfer_%llu_fallback;\n",
              map_misalignment, map, map_unaligned, map_misalignment,
              map_unaligned, (unsigned long long)label_id);

    const char *shared_address =
        desc->direction == MTLC_TENSOR_TRANSFER_GLOBAL_TO_WORKGROUP
            ? destination
            : source;
    int r_shared_misalignment = new_reg(fn, PC_B64);
    int p_shared_unaligned = new_reg(fn, PC_PRED);
    char shared_misalignment[24], shared_unaligned[24];
    reg_name(PC_B64, r_shared_misalignment, shared_misalignment);
    reg_name(PC_PRED, p_shared_unaligned, shared_unaligned);
    sb_printf(&fn->body,
              "\tand.b64 %s, %s, 15;\n"
              "\tsetp.ne.u64 %s, %s, 0;\n"
              "\t@%s bra mtlc_tensor_transfer_%llu_fallback;\n",
              shared_misalignment, shared_address, shared_unaligned,
              shared_misalignment, shared_unaligned,
              (unsigned long long)label_id);

    int r_tid_x = new_reg(fn, PC_B32), r_tid_y = new_reg(fn, PC_B32);
    int r_tid_z = new_reg(fn, PC_B32), r_election = new_reg(fn, PC_B32);
    int p_elected = new_reg(fn, PC_PRED);
    char tid_x[24], tid_y[24], tid_z[24], election[24], elected[24];
    reg_name(PC_B32, r_tid_x, tid_x);
    reg_name(PC_B32, r_tid_y, tid_y);
    reg_name(PC_B32, r_tid_z, tid_z);
    reg_name(PC_B32, r_election, election);
    reg_name(PC_PRED, p_elected, elected);
    sb_printf(&fn->body,
              "\tmov.u32 %s, %%tid.x;\n"
              "\tmov.u32 %s, %%tid.y;\n"
              "\tmov.u32 %s, %%tid.z;\n"
              "\tor.b32 %s, %s, %s;\n"
              "\tor.b32 %s, %s, %s;\n"
              "\tsetp.eq.u32 %s, %s, 0;\n",
              tid_x, tid_y, tid_z, election, tid_x, tid_y, election, election,
              tid_z, elected, election);
    char coordinates[MTLC_TENSOR_MAX_RANK][24];
    for (uint8_t dimension = 0; dimension < desc->rank; dimension++)
      use_as(fn, &in->arguments[coordinate_base + dimension], PC_B32,
             coordinates[dimension]);
    char coordinate_text[192] = {0};
    size_t used = 0;
    for (uint8_t dimension = 0; dimension < desc->rank; dimension++) {
      int wrote = snprintf(coordinate_text + used, sizeof(coordinate_text) - used,
                           "%s%s", dimension ? ", " : "",
                           coordinates[dimension]);
      if (wrote < 0 || (size_t)wrote >= sizeof(coordinate_text) - used) {
        fn_error(fn, "PTX tensor transfer coordinate list overflowed");
        return;
      }
      used += (size_t)wrote;
    }
    sb_printf(&fn->body,
              "\t// mtlc.tensor_transfer native-tma rank=%u bytes=%llu\n",
              (unsigned)desc->rank,
              (unsigned long long)(ir_tensor_transfer_tile_elements(desc) *
                                   ir_tensor_transfer_element_bytes(
                                       desc->element)));
    sb_printf(&fn->body,
              "\t@%s fence.proxy.tensormap::generic.acquire.sys [%s], 128;\n",
              elected, map);
    if (desc->direction == MTLC_TENSOR_TRANSFER_GLOBAL_TO_WORKGROUP) {
      char barrier_name[512], barrier[24], state[24], wait[24];
      ptx_tensor_transfer_barrier_name(fn, barrier_name, sizeof(barrier_name));
      int r_barrier = new_reg(fn, PC_B32);
      int r_state = new_reg(fn, PC_B64);
      int p_wait = new_reg(fn, PC_PRED);
      reg_name(PC_B32, r_barrier, barrier);
      reg_name(PC_B64, r_state, state);
      reg_name(PC_PRED, p_wait, wait);
      sb_printf(&fn->body,
                "\tmov.u32 %s, %s;\n"
                "\t@%s mbarrier.init.shared::cta.b64 [%s], 1;\n"
                "\t@%s fence.proxy.async.shared::cta;\n"
                "\tbar.sync 0;\n"
                "\t@%s cp.async.bulk.tensor.%ud.%s.global.tile.mbarrier::complete_tx::bytes [%s], [%s, {%s}], [%s];\n"
                "\t@%s mbarrier.arrive.expect_tx.release.cta.shared::cta.b64 %s, [%s], %llu;\n"
                "mtlc_tensor_transfer_%llu_wait:\n"
                "\tmbarrier.try_wait.parity.acquire.cta.shared::cta.b64 %s, [%s], 0;\n"
                "\t@!%s bra mtlc_tensor_transfer_%llu_wait;\n"
                "\tbar.sync 0;\n"
                "\t@%s mbarrier.inval.shared::cta.b64 [%s];\n",
                barrier, barrier_name, elected, barrier, elected, elected,
                (unsigned)desc->rank,
                ptx_version_at_least(fn, 8, 6) ? "shared::cta"
                                               : "shared::cluster",
                destination, map, coordinate_text,
                barrier, elected, state, barrier,
                (unsigned long long)(ir_tensor_transfer_tile_elements(desc) *
                                     ir_tensor_transfer_element_bytes(
                                         desc->element)),
                (unsigned long long)label_id, wait, barrier, wait,
                (unsigned long long)label_id, elected, barrier);
    } else {
      sb_printf(&fn->body,
                "\tfence.proxy.async.shared::cta;\n"
                "\tbar.sync 0;\n"
                "\t@%s cp.async.bulk.tensor.%ud.global.shared::cta.tile.bulk_group [%s, {%s}], [%s];\n"
                "\t@%s cp.async.bulk.commit_group;\n"
                "\t@%s cp.async.bulk.wait_group 0;\n"
                "\tbar.sync 0;\n",
                elected, (unsigned)desc->rank, map, coordinate_text, source,
                elected, elected);
    }
    sb_printf(&fn->body, "\tbra mtlc_tensor_transfer_%llu_done;\n",
              (unsigned long long)label_id);
    sb_printf(&fn->body, "mtlc_tensor_transfer_%llu_fallback:\n",
              (unsigned long long)label_id);
  }
  ptx_emit_tensor_transfer_fallback(fn, in, destination, source,
                                    coordinate_base, label_id);
  if (native)
    sb_printf(&fn->body, "mtlc_tensor_transfer_%llu_done:\n",
              (unsigned long long)label_id);
}

static const char *ptx_tensor_layout(MtlcTensorLayout layout) {
  if (layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR) return "row";
  if (layout == MTLC_TENSOR_LAYOUT_COLUMN_MAJOR) return "col";
  return NULL;
}

static int ptx_tensor_shape_is(const MtlcTensorMmaDesc *desc,
                               unsigned m, unsigned n, unsigned k) {
  return desc->m == m && desc->n == n && desc->k == k;
}

static void ptx_wmma_set_shape(const MtlcTensorMmaDesc *desc,
                               PtxWmmaProfile *profile,
                               const char *shape, unsigned m,
                               unsigned n, unsigned k) {
  profile->shape = shape;
  profile->tile_m = m;
  profile->tile_n = n;
  profile->tile_k = k;
  profile->m_tiles = desc->m / m;
  profile->n_tiles = desc->n / n;
}

static int ptx_wmma_shape_or_grid(const MtlcTensorMmaDesc *desc,
                                  PtxWmmaProfile *profile,
                                  const char *shape, unsigned m,
                                  unsigned n, unsigned k) {
  if (!desc || !profile || desc->k != k || desc->m == 0 || desc->n == 0 ||
      desc->m % m != 0 || desc->n % n != 0 ||
      desc->m > 256 || desc->n > 256)
    return 0;
  ptx_wmma_set_shape(desc, profile, shape, m, n, k);
  return 1;
}

static int ptx_wmma_shape_16_family(const MtlcTensorMmaDesc *desc,
                                     PtxWmmaProfile *profile) {
  if (ptx_tensor_shape_is(desc, 16, 16, 16)) {
    ptx_wmma_set_shape(desc, profile, "m16n16k16", 16, 16, 16);
    return 1;
  }
  if (ptx_tensor_shape_is(desc, 8, 32, 16)) {
    ptx_wmma_set_shape(desc, profile, "m8n32k16", 8, 32, 16);
    return 1;
  }
  if (ptx_tensor_shape_is(desc, 32, 8, 16)) {
    ptx_wmma_set_shape(desc, profile, "m32n8k16", 32, 8, 16);
    return 1;
  }
  return ptx_wmma_shape_or_grid(desc, profile, "m16n16k16",
                                16, 16, 16);
}

static int ptx_tensor_uses_narrow_float(const MtlcTensorMmaDesc *desc) {
  if (!desc) return 0;
  return (desc->a_element >= MTLC_TENSOR_ELEMENT_FLOAT8_E4M3 &&
          desc->a_element <= MTLC_TENSOR_ELEMENT_FLOAT4_E2M1) ||
         (desc->b_element >= MTLC_TENSOR_ELEMENT_FLOAT8_E4M3 &&
          desc->b_element <= MTLC_TENSOR_ELEMENT_FLOAT4_E2M1);
}

// 8-bit integer A and B whose blocks carry floating-point scales: the
// block dot products are exact in int32, then scaled per (row, column) in
// f32 -- the shape of a quantized weight block against quantized
// activations.
static int ptx_tensor_is_float_scaled_int8(const MtlcTensorMmaDesc *desc) {
  if (!desc) return 0;
  int a_int8 = desc->a_element == MTLC_TENSOR_ELEMENT_INT8 ||
               desc->a_element == MTLC_TENSOR_ELEMENT_UINT8;
  int b_int8 = desc->b_element == MTLC_TENSOR_ELEMENT_INT8 ||
               desc->b_element == MTLC_TENSOR_ELEMENT_UINT8 ||
               ((desc->b_element == MTLC_TENSOR_ELEMENT_INT4 ||
                 desc->b_element == MTLC_TENSOR_ELEMENT_UINT4) &&
                desc->b_packing == MTLC_TENSOR_PACKING_HALVES);
  int a_float_scale =
      desc->a_scale_mode != MTLC_TENSOR_SCALE_NONE &&
      (desc->a_scale_element == MTLC_TENSOR_ELEMENT_FLOAT32 ||
       desc->a_scale_element == MTLC_TENSOR_ELEMENT_FLOAT16);
  int b_float_scale =
      desc->b_scale_mode != MTLC_TENSOR_SCALE_NONE &&
      (desc->b_scale_element == MTLC_TENSOR_ELEMENT_FLOAT32 ||
       desc->b_scale_element == MTLC_TENSOR_ELEMENT_FLOAT16);
  return a_int8 && b_int8 && (a_float_scale || b_float_scale);
}

// A dense, unscaled f16 or bf16 tile with f32 accumulation that stable
// WMMA cannot take in one operation -- K above 16 -- runs on the native
// m16n8k16 MMA the C row scale uses, unrolled over K with its accumulators
// in registers, rather than being refused. K = 16 tiles keep WMMA.
static int ptx_tensor_is_f16_native(const MtlcTensorMmaDesc *desc) {
  return desc && desc->c_scale_mode == MTLC_TENSOR_SCALE_NONE &&
         desc->sparsity == MTLC_TENSOR_SPARSITY_DENSE &&
         desc->math_mode == MTLC_TENSOR_MATH_MULTIPLY_ADD &&
         desc->a_scale_mode == MTLC_TENSOR_SCALE_NONE &&
         desc->b_scale_mode == MTLC_TENSOR_SCALE_NONE &&
         desc->a_element == desc->b_element &&
         (desc->a_element == MTLC_TENSOR_ELEMENT_FLOAT16 ||
          desc->a_element == MTLC_TENSOR_ELEMENT_BFLOAT16) &&
         desc->accumulator_element == MTLC_TENSOR_ELEMENT_FLOAT32 &&
         desc->result_element == MTLC_TENSOR_ELEMENT_FLOAT32 &&
         desc->a_packing == MTLC_TENSOR_PACKING_LOGICAL &&
         desc->b_packing == MTLC_TENSOR_PACKING_LOGICAL &&
         desc->k > 16 && desc->k % 16 == 0 && desc->m % 16 == 0 &&
         desc->n % 8 == 0;
}

static int ptx_tensor_uses_direct_mma(const MtlcTensorMmaDesc *desc) {
  return desc &&
         (desc->c_scale_mode != MTLC_TENSOR_SCALE_NONE ||
          desc->sparsity != MTLC_TENSOR_SPARSITY_DENSE ||
          ptx_tensor_uses_narrow_float(desc) ||
          ptx_tensor_is_float_scaled_int8(desc) ||
          ptx_tensor_is_f16_native(desc));
}

static const char *ptx_mma_fp8_type(MtlcTensorElement element) {
  switch (element) {
  case MTLC_TENSOR_ELEMENT_FLOAT8_E4M3:
    return "e4m3";
  case MTLC_TENSOR_ELEMENT_FLOAT8_E5M2:
    return "e5m2";
  default:
    return NULL;
  }
}

static const char *ptx_mma_mxf8f6f4_type(MtlcTensorElement element,
                                         int *bits) {
  if (bits) *bits = 0;
  switch (element) {
  case MTLC_TENSOR_ELEMENT_FLOAT8_E4M3:
    if (bits) *bits = 8;
    return "e4m3";
  case MTLC_TENSOR_ELEMENT_FLOAT8_E5M2:
    if (bits) *bits = 8;
    return "e5m2";
  case MTLC_TENSOR_ELEMENT_FLOAT6_E2M3:
    if (bits) *bits = 6;
    return "e2m3";
  case MTLC_TENSOR_ELEMENT_FLOAT6_E3M2:
    if (bits) *bits = 6;
    return "e3m2";
  case MTLC_TENSOR_ELEMENT_FLOAT4_E2M1:
    if (bits) *bits = 4;
    return "e2m1";
  default:
    return NULL;
  }
}

static int ptx_select_mma_profile(PtxFn *fn,
                                  const MtlcTensorMmaDesc *desc,
                                  PtxMmaProfile *profile,
                                  char *reason, size_t reason_size) {
  memset(profile, 0, sizeof(*profile));
#define PTX_MMA_REJECT(...)                                                    \
  do {                                                                         \
    snprintf(reason, reason_size, __VA_ARGS__);                                \
    return 0;                                                                  \
  } while (0)
  if (!fn || !ir_tensor_mma_desc_valid(desc))
    PTX_MMA_REJECT("invalid target-neutral tensor descriptor");
  if ((desc->a_swizzle || desc->b_swizzle) &&
      !ptx_tensor_is_float_scaled_int8(desc))
    PTX_MMA_REJECT("swizzled operands are offered on block-scaled i8 tiles");
  if (desc->scope != MTLC_MEMORY_SCOPE_SUBGROUP)
    PTX_MMA_REJECT("warp-level MMA requires subgroup scope");
  if (desc->c_scale_mode != MTLC_TENSOR_SCALE_NONE ||
      ptx_tensor_is_f16_native(desc)) {
    if (desc->c_scale_mode != MTLC_TENSOR_SCALE_NONE &&
        desc->c_scale_mode != MTLC_TENSOR_SCALE_PER_ROW)
      PTX_MMA_REJECT("the C scale is per row");
    if (desc->math_mode != MTLC_TENSOR_MATH_MULTIPLY_ADD ||
        desc->sparsity != MTLC_TENSOR_SPARSITY_DENSE ||
        desc->a_scale_mode != MTLC_TENSOR_SCALE_NONE ||
        desc->b_scale_mode != MTLC_TENSOR_SCALE_NONE)
      PTX_MMA_REJECT("a C row scale composes with dense, unscaled multiply-add only");
    if (desc->a_element != desc->b_element ||
        (desc->a_element != MTLC_TENSOR_ELEMENT_FLOAT16 &&
         desc->a_element != MTLC_TENSOR_ELEMENT_BFLOAT16))
      PTX_MMA_REJECT("a C row scale is offered on matching f16 or bf16 inputs");
    if (desc->a_packing != MTLC_TENSOR_PACKING_LOGICAL ||
        desc->b_packing != MTLC_TENSOR_PACKING_LOGICAL)
      PTX_MMA_REJECT("f16/bf16 operands are logically packed");
    if (desc->k % 16 != 0 || desc->m % 16 != 0 || desc->n % 8 != 0)
      PTX_MMA_REJECT("f16/bf16 native tiles take M divisible by 16, N by 8, K by 16");
    if (desc->m > 256 || desc->n > 256)
      PTX_MMA_REJECT("one f16/bf16 native collective tile is limited to at most 256x256");
    if (desc->rounding != MTLC_TENSOR_ROUND_DEFAULT ||
        desc->overflow != MTLC_TENSOR_OVERFLOW_WRAP)
      PTX_MMA_REJECT("f16/bf16 MMA accumulates in f32 with default rounding");
    if (fn->target_arch < 80 || !ptx_version_at_least(fn, 7, 0))
      PTX_MMA_REJECT("f16/bf16 mma.sync m16n8k16 requires PTX 7.0 and sm_80 or newer");
    profile->kind = PTX_MMA_F16;
    profile->c_row_scaled = desc->c_scale_mode != MTLC_TENSOR_SCALE_NONE;
    profile->shape = "m16n8k16";
    profile->a_type = profile->b_type =
        desc->a_element == MTLC_TENSOR_ELEMENT_FLOAT16 ? "f16" : "bf16";
    profile->c_type = profile->d_type = "f32";
    profile->a_bits = profile->b_bits = 16;
    profile->a_registers = 4;
    profile->b_registers = 2;
    profile->accumulator_registers = 4;
    profile->m_tiles = desc->m / 16;
    profile->n_tiles = desc->n / 8;
    return 1;
  }
  if (desc->sparsity != MTLC_TENSOR_SPARSITY_DENSE) {
    if (desc->sparsity != MTLC_TENSOR_SPARSITY_STRUCTURED_2_TO_4)
      PTX_MMA_REJECT("this PTX sparse MMA profile requires canonical structured 2:4 A");
    if (desc->math_mode != MTLC_TENSOR_MATH_MULTIPLY_ADD)
      PTX_MMA_REJECT("structured-sparse MMA requires multiply-add math");
    if (desc->a_element != desc->b_element ||
        (desc->a_element != MTLC_TENSOR_ELEMENT_FLOAT16 &&
         desc->a_element != MTLC_TENSOR_ELEMENT_BFLOAT16))
      PTX_MMA_REJECT("this PTX sparse MMA profile requires matching f16 or bf16 inputs");
    if (desc->accumulator_element != MTLC_TENSOR_ELEMENT_FLOAT32 ||
        desc->result_element != MTLC_TENSOR_ELEMENT_FLOAT32)
      PTX_MMA_REJECT("this PTX sparse MMA profile requires f32 accumulation and result");
    if (desc->m % 16 != 0 || desc->n % 8 != 0 || desc->k != 16)
      PTX_MMA_REJECT("f16/bf16 structured-sparse tiles require M divisible by 16, N divisible by 8, and K=16");
    if (desc->m > 256 || desc->n > 256)
      PTX_MMA_REJECT("one structured-sparse collective tile is limited to at most 256x256");
    if (desc->rounding != MTLC_TENSOR_ROUND_DEFAULT ||
        desc->overflow != MTLC_TENSOR_OVERFLOW_WRAP)
      PTX_MMA_REJECT("structured-sparse f16/bf16 MMA does not preserve explicit rounding or saturation");
    if (desc->a_scale_mode != MTLC_TENSOR_SCALE_NONE ||
        desc->b_scale_mode != MTLC_TENSOR_SCALE_NONE)
      PTX_MMA_REJECT("this structured-sparse profile is unscaled");
    if (desc->a_packing != MTLC_TENSOR_PACKING_LOGICAL ||
        desc->b_packing != MTLC_TENSOR_PACKING_LOGICAL)
      PTX_MMA_REJECT("structured-sparse f16/bf16 storage uses logical element packing");
    if (fn->target_arch < 80 || !ptx_version_at_least(fn, 7, 1))
      PTX_MMA_REJECT("structured-sparse mma.sp requires PTX 7.1 and sm_80 or newer");

    profile->kind = desc->a_element == MTLC_TENSOR_ELEMENT_FLOAT16
                        ? PTX_MMA_SPARSE_F16
                        : PTX_MMA_SPARSE_BF16;
    profile->shape = "m16n8k16";
    profile->a_type = profile->b_type =
        desc->a_element == MTLC_TENSOR_ELEMENT_FLOAT16 ? "f16" : "bf16";
    profile->c_type = profile->d_type = "f32";
    profile->a_bits = profile->b_bits = 16;
    profile->a_registers = profile->b_registers = 2;
    profile->accumulator_registers = 4;
    profile->m_tiles = desc->m / 16;
    profile->n_tiles = desc->n / 8;
    return 1;
  }
  if (ptx_tensor_is_float_scaled_int8(desc)) {
    if (desc->math_mode != MTLC_TENSOR_MATH_MULTIPLY_ADD)
      PTX_MMA_REJECT("block-scaled i8 MMA requires multiply-add math");
    if (desc->a_scale_mode != MTLC_TENSOR_SCALE_BLOCK_32 ||
        desc->b_scale_mode != MTLC_TENSOR_SCALE_BLOCK_32 ||
        (desc->a_scale_element != MTLC_TENSOR_ELEMENT_FLOAT32 &&
         desc->a_scale_element != MTLC_TENSOR_ELEMENT_FLOAT16) ||
        (desc->b_scale_element != MTLC_TENSOR_ELEMENT_FLOAT32 &&
         desc->b_scale_element != MTLC_TENSOR_ELEMENT_FLOAT16))
      PTX_MMA_REJECT("block-scaled i8 MMA requires f32 or f16 block32 scales on both A and B");
    if (desc->k % 32 != 0 || desc->k > 256)
      PTX_MMA_REJECT("block-scaled i8 native tiles take K in whole 32-blocks, at most 256: one scale block per 32");
    if (desc->accumulator_element != MTLC_TENSOR_ELEMENT_FLOAT32 ||
        desc->result_element != MTLC_TENSOR_ELEMENT_FLOAT32)
      PTX_MMA_REJECT("block-scaled i8 MMA accumulates and returns f32");
    if (desc->m % 16 != 0 || desc->n % 8 != 0)
      PTX_MMA_REJECT("block-scaled i8 tiles require M divisible by 16 and N divisible by 8");
    if (desc->m > 256 || desc->n > 256)
      PTX_MMA_REJECT("one block-scaled i8 collective tile is limited to at most 256x256");
    if (desc->rounding != MTLC_TENSOR_ROUND_DEFAULT ||
        desc->overflow != MTLC_TENSOR_OVERFLOW_WRAP)
      PTX_MMA_REJECT("block-scaled i8 MMA has an exact int32 block dot and default f32 rounding; no other rounding or saturation is offered");
    int b_nibbles = desc->b_element == MTLC_TENSOR_ELEMENT_INT4 ||
                    desc->b_element == MTLC_TENSOR_ELEMENT_UINT4;
    if (desc->a_packing != MTLC_TENSOR_PACKING_LOGICAL ||
        (!b_nibbles && desc->b_packing != MTLC_TENSOR_PACKING_LOGICAL))
      PTX_MMA_REJECT("block-scaled i8 operands use one logical byte per element, or halves-packed 4-bit B");
    if (b_nibbles && (desc->b_layout != MTLC_TENSOR_LAYOUT_COLUMN_MAJOR ||
                      desc->transpose_b))
      PTX_MMA_REJECT("halves-packed 4-bit B must be column-major and untransposed: each column's K run is whole 16-byte groups");
    if (desc->a_zero_point || (!b_nibbles && desc->b_zero_point))
      PTX_MMA_REJECT("block-scaled i8 MMA takes a zero point only on 4-bit B");
    if (fn->target_arch < 80 || !ptx_version_at_least(fn, 7, 0))
      PTX_MMA_REJECT("block-scaled i8 mma.sync m16n8k32 requires PTX 7.0 and sm_80 or newer");
    profile->kind = PTX_MMA_S8_SCALED;
    profile->shape = "m16n8k32";
    profile->a_type = desc->a_element == MTLC_TENSOR_ELEMENT_INT8 ? "s8" : "u8";
    profile->b_type = desc->b_element == MTLC_TENSOR_ELEMENT_INT8 ? "s8" : "u8";
    if (b_nibbles) {
      // Widened to bytes in registers: signed nibbles and offset ones
      // become s8; plain unsigned nibbles stay u8.
      profile->b_halves = 1;
      profile->b_signed4 = desc->b_element == MTLC_TENSOR_ELEMENT_INT4;
      profile->b_zero_point = desc->b_zero_point;
      profile->b_type =
          profile->b_signed4 || profile->b_zero_point ? "s8" : "u8";
    }
    profile->c_type = profile->d_type = "s32";
    profile->scale_vectors = 1;
    profile->a_bits = profile->b_bits = 8;
    profile->a_registers = 4;
    profile->b_registers = 2;
    profile->accumulator_registers = 4;
    profile->m_tiles = desc->m / 16;
    profile->n_tiles = desc->n / 8;
    return 1;
  }
  if (desc->math_mode != MTLC_TENSOR_MATH_MULTIPLY_ADD)
    PTX_MMA_REJECT("narrow floating-point MMA requires multiply-add math");
  if (desc->rounding != MTLC_TENSOR_ROUND_DEFAULT)
    PTX_MMA_REJECT("FP8 MMA does not preserve an explicit rounding request");
  if (desc->overflow != MTLC_TENSOR_OVERFLOW_WRAP)
    PTX_MMA_REJECT("FP8 MMA does not provide saturating accumulation");
  if (desc->accumulator_element != MTLC_TENSOR_ELEMENT_FLOAT32 ||
      desc->result_element != MTLC_TENSOR_ELEMENT_FLOAT32)
    PTX_MMA_REJECT("the current narrow-float native path requires f32 accumulation and result");
  if (desc->m % 16 != 0 || desc->n % 8 != 0)
    PTX_MMA_REJECT("native narrow-float tiles require M divisible by 16 and N divisible by 8");
  if (desc->m > 256 || desc->n > 256)
    PTX_MMA_REJECT("one native narrow-float collective tile is limited to at most 256x256");

  int mxf_a_bits = 0, mxf_b_bits = 0;
  const char *mxf_a_type =
      ptx_mma_mxf8f6f4_type(desc->a_element, &mxf_a_bits);
  const char *mxf_b_type =
      ptx_mma_mxf8f6f4_type(desc->b_element, &mxf_b_bits);
  int requests_mxf8f6f4 =
      desc->a_scale_mode == MTLC_TENSOR_SCALE_BLOCK_32 &&
      desc->b_scale_mode == MTLC_TENSOR_SCALE_BLOCK_32 &&
      desc->a_scale_element == MTLC_TENSOR_ELEMENT_SCALE_UE8M0 &&
      desc->b_scale_element == MTLC_TENSOR_ELEMENT_SCALE_UE8M0 &&
      (desc->k == 32 || mxf_a_bits == 6 || mxf_b_bits == 6);
  if (requests_mxf8f6f4) {
    if (!mxf_a_type || !mxf_b_type)
      PTX_MMA_REJECT("block-scaled mxf8f6f4 requires FP8, FP6, or FP4 inputs");
    if (desc->k != 32)
      PTX_MMA_REJECT("block-scaled mxf8f6f4 native tiles require K=32");
    if ((mxf_a_bits == 8 &&
         desc->a_packing != MTLC_TENSOR_PACKING_LOGICAL) ||
        (mxf_b_bits == 8 &&
         desc->b_packing != MTLC_TENSOR_PACKING_LOGICAL))
      PTX_MMA_REJECT("FP8 operands use one logical byte per element");
    if ((mxf_a_bits < 8 &&
         desc->a_packing != MTLC_TENSOR_PACKING_LOGICAL &&
         desc->a_packing != MTLC_TENSOR_PACKING_DENSE_SUBBYTE) ||
        (mxf_b_bits < 8 &&
         desc->b_packing != MTLC_TENSOR_PACKING_LOGICAL &&
         desc->b_packing != MTLC_TENSOR_PACKING_DENSE_SUBBYTE))
      PTX_MMA_REJECT("FP6/FP4 operands require logical or dense-subbyte packing");
    if (fn->target_arch < 120 ||
        (fn->target_variant != 'a' && fn->target_variant != 'f') ||
        !ptx_version_at_least(fn, 8, 8))
      PTX_MMA_REJECT("block-scaled mxf8f6f4 mma.sync requires PTX 8.8 and an architecture- or family-specific sm_120a/sm_121a target");

    profile->kind = PTX_MMA_MXF8F6F4;
    profile->shape = "m16n8k32";
    profile->a_type = mxf_a_type;
    profile->b_type = mxf_b_type;
    profile->c_type = profile->d_type = "f32";
    profile->scale_type = "ue8m0";
    profile->scale_vectors = 1;
    profile->a_bits = mxf_a_bits;
    profile->b_bits = mxf_b_bits;
    profile->a_registers = 4;
    profile->b_registers = 2;
    profile->accumulator_registers = 4;
    profile->m_tiles = desc->m / 16;
    profile->n_tiles = desc->n / 8;
    return 1;
  }

  if (desc->a_element == MTLC_TENSOR_ELEMENT_FLOAT4_E2M1 &&
      desc->b_element == MTLC_TENSOR_ELEMENT_FLOAT4_E2M1) {
    if (desc->k != 64)
      PTX_MMA_REJECT("MXFP4 native tiles require K=64");
    if (desc->a_packing != MTLC_TENSOR_PACKING_DENSE_SUBBYTE ||
        desc->b_packing != MTLC_TENSOR_PACKING_DENSE_SUBBYTE)
      PTX_MMA_REJECT("MXFP4 requires densely packed E2M1 operands");
    int is_mxfp4 =
        desc->a_scale_mode == MTLC_TENSOR_SCALE_BLOCK_32 &&
        desc->b_scale_mode == MTLC_TENSOR_SCALE_BLOCK_32 &&
        desc->a_scale_element == MTLC_TENSOR_ELEMENT_SCALE_UE8M0 &&
        desc->b_scale_element == MTLC_TENSOR_ELEMENT_SCALE_UE8M0;
    int is_nvfp4 =
        desc->a_scale_mode == MTLC_TENSOR_SCALE_BLOCK_16 &&
        desc->b_scale_mode == MTLC_TENSOR_SCALE_BLOCK_16 &&
        desc->a_scale_element == MTLC_TENSOR_ELEMENT_SCALE_UE4M3 &&
        desc->b_scale_element == MTLC_TENSOR_ELEMENT_SCALE_UE4M3;
    if (!is_mxfp4 && !is_nvfp4)
      PTX_MMA_REJECT("native FP4 requires matched UE8M0 block32 (MXFP4) or UE4M3 block16 (NVFP4) scales");
    if (fn->target_arch < 120 ||
        (fn->target_variant != 'a' && fn->target_variant != 'f') ||
        !ptx_version_at_least(fn, 8, 8))
      PTX_MMA_REJECT("native FP4 block-scale mma.sync requires PTX 8.8 and an architecture- or family-specific sm_120a/sm_121a target");

    profile->kind = is_nvfp4 ? PTX_MMA_NVFP4 : PTX_MMA_MXFP4;
    profile->shape = "m16n8k64";
    profile->a_type = profile->b_type = "e2m1";
    profile->c_type = profile->d_type = "f32";
    profile->scale_type = is_nvfp4 ? "ue4m3" : "ue8m0";
    profile->scale_vectors = is_nvfp4 ? 4 : 2;
    profile->a_bits = profile->b_bits = 4;
    profile->a_registers = 4;
    profile->b_registers = 2;
    profile->accumulator_registers = 4;
    profile->m_tiles = desc->m / 16;
    profile->n_tiles = desc->n / 8;
    return 1;
  }

  if (desc->a_scale_mode != MTLC_TENSOR_SCALE_NONE ||
      desc->b_scale_mode != MTLC_TENSOR_SCALE_NONE)
    PTX_MMA_REJECT("this native narrow-float profile does not support the requested scaling mode");
  if (!ptx_mma_fp8_type(desc->a_element) ||
      !ptx_mma_fp8_type(desc->b_element))
    PTX_MMA_REJECT("this native MMA profile requires FP8 e4m3/e5m2 or packed MXFP4 inputs");
  if (desc->k != 16 && desc->k != 32)
    PTX_MMA_REJECT("FP8 native tiles require K=16 or K=32");
  if (fn->target_arch < 89 || !ptx_version_at_least(fn, 8, 4))
    PTX_MMA_REJECT("FP8 mma.sync requires PTX 8.4 and sm_89 or newer");

  profile->kind = PTX_MMA_FP8;
  profile->shape = desc->k == 16 ? "m16n8k16" : "m16n8k32";
  profile->a_type = ptx_mma_fp8_type(desc->a_element);
  profile->b_type = ptx_mma_fp8_type(desc->b_element);
  profile->c_type = profile->d_type = "f32";
  profile->a_bits = profile->b_bits = 8;
  profile->a_registers = desc->k == 16 ? 2 : 4;
  profile->b_registers = desc->k == 16 ? 1 : 2;
  profile->accumulator_registers = 4;
  profile->m_tiles = desc->m / 16;
  profile->n_tiles = desc->n / 8;
  return 1;
#undef PTX_MMA_REJECT
}

static int ptx_select_wmma_profile(PtxFn *fn,
                                   const MtlcTensorMmaDesc *desc,
                                   PtxWmmaProfile *profile,
                                   char *reason, size_t reason_size) {
  memset(profile, 0, sizeof(*profile));
#define PTX_WMMA_REJECT(...)                                                   \
  do {                                                                         \
    snprintf(reason, reason_size, __VA_ARGS__);                                \
    return 0;                                                                  \
  } while (0)
  if (!ir_tensor_mma_desc_valid(desc)) {
    PTX_WMMA_REJECT("invalid target-neutral tensor descriptor");
  }
  if (desc->scope != MTLC_MEMORY_SCOPE_SUBGROUP) {
    PTX_WMMA_REJECT("WMMA requires subgroup scope");
  }
  if (desc->sparsity != MTLC_TENSOR_SPARSITY_DENSE) {
    PTX_WMMA_REJECT("structured sparsity requires the PTX mma/tcgen path");
  }
  if (desc->a_scale_mode != MTLC_TENSOR_SCALE_NONE ||
      desc->b_scale_mode != MTLC_TENSOR_SCALE_NONE) {
    PTX_WMMA_REJECT("scaled tensor formats require the PTX mma/tcgen path");
  }
  if (desc->transpose_a || desc->transpose_b) {
    PTX_WMMA_REJECT("explicit transpose requires a tiled transform or mma path");
  }
  if (!ptx_tensor_layout(desc->a_layout) ||
      !ptx_tensor_layout(desc->b_layout) ||
      !ptx_tensor_layout(desc->c_layout) ||
      !ptx_tensor_layout(desc->d_layout)) {
    PTX_WMMA_REJECT("invalid tensor layout");
  }

  profile->a_class = PC_B32;
  profile->b_class = PC_B32;
  profile->c_class = PC_B32;
  profile->d_class = PC_B32;
  if (desc->a_element == MTLC_TENSOR_ELEMENT_FLOAT16 &&
      desc->b_element == MTLC_TENSOR_ELEMENT_FLOAT16 &&
      (desc->accumulator_element == MTLC_TENSOR_ELEMENT_FLOAT16 ||
       desc->accumulator_element == MTLC_TENSOR_ELEMENT_FLOAT32) &&
      (desc->result_element == MTLC_TENSOR_ELEMENT_FLOAT16 ||
       desc->result_element == MTLC_TENSOR_ELEMENT_FLOAT32) &&
      desc->math_mode == MTLC_TENSOR_MATH_MULTIPLY_ADD &&
      ptx_wmma_shape_16_family(desc, profile)) {
    profile->kind = PTX_WMMA_F16;
    profile->a_type = profile->b_type = "f16";
    profile->c_type = desc->accumulator_element == MTLC_TENSOR_ELEMENT_FLOAT16
                          ? "f16"
                          : "f32";
    profile->d_type = desc->result_element == MTLC_TENSOR_ELEMENT_FLOAT16
                          ? "f16"
                          : "f32";
    profile->a_registers = profile->b_registers = 8;
    profile->c_registers =
        desc->accumulator_element == MTLC_TENSOR_ELEMENT_FLOAT16 ? 4 : 8;
    profile->d_registers =
        desc->result_element == MTLC_TENSOR_ELEMENT_FLOAT16 ? 4 : 8;
    profile->c_class = desc->accumulator_element == MTLC_TENSOR_ELEMENT_FLOAT32
                           ? PC_F32
                           : PC_B32;
    profile->d_class = desc->result_element == MTLC_TENSOR_ELEMENT_FLOAT32
                           ? PC_F32
                           : PC_B32;
    profile->min_arch = 70;
    profile->min_ptx_major = 6;
    profile->min_ptx_minor =
        profile->tile_m == 16 && profile->tile_n == 16 ? 0 : 1;
  } else if (desc->a_element == MTLC_TENSOR_ELEMENT_BFLOAT16 &&
             desc->b_element == MTLC_TENSOR_ELEMENT_BFLOAT16 &&
             desc->accumulator_element == MTLC_TENSOR_ELEMENT_FLOAT32 &&
             desc->result_element == MTLC_TENSOR_ELEMENT_FLOAT32 &&
             desc->math_mode == MTLC_TENSOR_MATH_MULTIPLY_ADD &&
             ptx_wmma_shape_16_family(desc, profile)) {
    profile->kind = PTX_WMMA_BF16;
    profile->a_type = profile->b_type = "bf16";
    profile->c_type = profile->d_type = "f32";
    profile->c_class = profile->d_class = PC_F32;
    if (profile->tile_m == 16 && profile->tile_n == 16) {
      profile->a_registers = profile->b_registers = 4;
    } else if (profile->tile_m == 8 && profile->tile_n == 32) {
      profile->a_registers = 2;
      profile->b_registers = 8;
    } else {
      profile->a_registers = 8;
      profile->b_registers = 2;
    }
    profile->c_registers = profile->d_registers = 8;
    profile->min_arch = 80;
    profile->min_ptx_major = 7;
  } else if (desc->a_element == MTLC_TENSOR_ELEMENT_TFLOAT32 &&
             desc->b_element == MTLC_TENSOR_ELEMENT_TFLOAT32 &&
             desc->accumulator_element == MTLC_TENSOR_ELEMENT_FLOAT32 &&
             desc->result_element == MTLC_TENSOR_ELEMENT_FLOAT32 &&
             desc->math_mode == MTLC_TENSOR_MATH_MULTIPLY_ADD &&
             ptx_wmma_shape_or_grid(desc, profile, "m16n16k8",
                                    16, 16, 8)) {
    profile->kind = PTX_WMMA_TF32;
    profile->a_type = profile->b_type = "tf32";
    profile->c_type = profile->d_type = "f32";
    profile->a_registers = profile->b_registers = 4;
    profile->c_registers = profile->d_registers = 8;
    profile->c_class = profile->d_class = PC_F32;
    profile->min_arch = 80;
    profile->min_ptx_major = 7;
  } else if (desc->a_element == MTLC_TENSOR_ELEMENT_FLOAT64 &&
             desc->b_element == MTLC_TENSOR_ELEMENT_FLOAT64 &&
             desc->accumulator_element == MTLC_TENSOR_ELEMENT_FLOAT64 &&
             desc->result_element == MTLC_TENSOR_ELEMENT_FLOAT64 &&
             desc->math_mode == MTLC_TENSOR_MATH_MULTIPLY_ADD &&
             ptx_wmma_shape_or_grid(desc, profile, "m8n8k4", 8, 8, 4)) {
    profile->kind = PTX_WMMA_F64;
    profile->a_type = profile->b_type = profile->c_type = profile->d_type =
        "f64";
    profile->a_class = profile->b_class = profile->c_class = profile->d_class =
        PC_F64;
    profile->a_registers = profile->b_registers = 1;
    profile->c_registers = profile->d_registers = 2;
    profile->min_arch = 80;
    profile->min_ptx_major = 7;
  } else if ((desc->a_element == MTLC_TENSOR_ELEMENT_INT8 ||
              desc->a_element == MTLC_TENSOR_ELEMENT_UINT8) &&
             desc->b_element == desc->a_element &&
             desc->accumulator_element == MTLC_TENSOR_ELEMENT_INT32 &&
             desc->result_element == MTLC_TENSOR_ELEMENT_INT32 &&
             desc->math_mode == MTLC_TENSOR_MATH_MULTIPLY_ADD &&
             ptx_wmma_shape_16_family(desc, profile)) {
    profile->kind = PTX_WMMA_I8;
    profile->a_type = profile->b_type =
        desc->a_element == MTLC_TENSOR_ELEMENT_INT8 ? "s8" : "u8";
    profile->c_type = profile->d_type = "s32";
    if (profile->tile_m == 16 && profile->tile_n == 16) {
      profile->a_registers = profile->b_registers = 2;
    } else if (profile->tile_m == 8 && profile->tile_n == 32) {
      profile->a_registers = 1;
      profile->b_registers = 4;
    } else {
      profile->a_registers = 4;
      profile->b_registers = 1;
    }
    profile->c_registers = profile->d_registers = 8;
    profile->min_arch = 72;
    profile->min_ptx_major = 6;
    profile->min_ptx_minor = 3;
  } else if ((desc->a_element == MTLC_TENSOR_ELEMENT_INT4 ||
              desc->a_element == MTLC_TENSOR_ELEMENT_UINT4) &&
             desc->b_element == desc->a_element &&
             desc->accumulator_element == MTLC_TENSOR_ELEMENT_INT32 &&
             desc->result_element == MTLC_TENSOR_ELEMENT_INT32 &&
             desc->math_mode == MTLC_TENSOR_MATH_MULTIPLY_ADD &&
             ptx_tensor_shape_is(desc, 8, 8, 32) &&
             desc->a_layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR &&
             desc->b_layout == MTLC_TENSOR_LAYOUT_COLUMN_MAJOR) {
    profile->kind = PTX_WMMA_I4;
    ptx_wmma_set_shape(desc, profile, "m8n8k32", 8, 8, 32);
    profile->a_type = profile->b_type =
        desc->a_element == MTLC_TENSOR_ELEMENT_INT4 ? "s4" : "u4";
    profile->c_type = profile->d_type = "s32";
    profile->a_registers = profile->b_registers = 1;
    profile->c_registers = profile->d_registers = 2;
    profile->min_arch = 75;
    profile->min_ptx_major = 6;
    profile->min_ptx_minor = 3;
  } else if (desc->a_element == MTLC_TENSOR_ELEMENT_BIT1 &&
             desc->b_element == MTLC_TENSOR_ELEMENT_BIT1 &&
             desc->accumulator_element == MTLC_TENSOR_ELEMENT_INT32 &&
             desc->result_element == MTLC_TENSOR_ELEMENT_INT32 &&
             (desc->math_mode == MTLC_TENSOR_MATH_XOR_POPCOUNT ||
              desc->math_mode == MTLC_TENSOR_MATH_AND_POPCOUNT) &&
             ptx_tensor_shape_is(desc, 8, 8, 128) &&
             desc->a_layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR &&
             desc->b_layout == MTLC_TENSOR_LAYOUT_COLUMN_MAJOR) {
    profile->kind = PTX_WMMA_B1;
    ptx_wmma_set_shape(desc, profile, "m8n8k128", 8, 8, 128);
    profile->a_type = profile->b_type = "b1";
    profile->c_type = profile->d_type = "s32";
    profile->a_registers = profile->b_registers = 1;
    profile->c_registers = profile->d_registers = 2;
    profile->min_arch = 75;
    profile->min_ptx_major = 6;
    profile->min_ptx_minor = 3;
  } else {
    PTX_WMMA_REJECT(
        "profile is not a stable PTX WMMA combination; it requires mma/tcgen lowering");
  }
  if (desc->overflow == MTLC_TENSOR_OVERFLOW_SATURATE_FINITE &&
      profile->kind != PTX_WMMA_I8 && profile->kind != PTX_WMMA_I4) {
    PTX_WMMA_REJECT("finite saturation is supported only for integer WMMA");
  }
  if (profile->kind != PTX_WMMA_F64 &&
      desc->rounding != MTLC_TENSOR_ROUND_DEFAULT) {
    PTX_WMMA_REJECT("explicit rounding is supported only for f64 WMMA");
  }
  if (profile->kind == PTX_WMMA_F64 &&
      desc->rounding != MTLC_TENSOR_ROUND_DEFAULT &&
      desc->rounding != MTLC_TENSOR_ROUND_NEAREST_EVEN) {
    PTX_WMMA_REJECT("this PTX WMMA lowering currently supports default/RN f64 rounding");
  }
  if (fn->target_arch < profile->min_arch ||
      !ptx_version_at_least(fn, profile->min_ptx_major,
                            profile->min_ptx_minor)) {
    PTX_WMMA_REJECT("profile requires PTX %d.%d and sm_%d or newer",
                    profile->min_ptx_major, profile->min_ptx_minor,
                    profile->min_arch);
  }
  return 1;
#undef PTX_WMMA_REJECT
}

static void ptx_reg_tuple(PtxFn *fn, PtxClass cls, int count,
                          char *buffer, size_t buffer_size) {
  size_t offset = 0;
  if (!buffer || buffer_size == 0) return;
  offset += (size_t)snprintf(buffer + offset, buffer_size - offset, "{");
  for (int i = 0; i < count && offset < buffer_size; i++) {
    char reg[24];
    reg_name(cls, new_reg(fn, cls), reg);
    offset += (size_t)snprintf(buffer + offset, buffer_size - offset,
                               "%s%s", i ? ", " : "", reg);
  }
  if (offset < buffer_size) snprintf(buffer + offset, buffer_size - offset, "}");
}

static void ptx_reg_tuple_at(PtxClass cls, int base, int count,
                             char *buffer, size_t buffer_size) {
  size_t offset = 0;
  if (!buffer || buffer_size == 0) return;
  offset += (size_t)snprintf(buffer + offset, buffer_size - offset, "{");
  for (int i = 0; i < count && offset < buffer_size; i++) {
    char reg[24];
    reg_name(cls, base + i, reg);
    offset += (size_t)snprintf(buffer + offset, buffer_size - offset,
                              "%s%s", i ? ", " : "", reg);
  }
  if (offset < buffer_size) snprintf(buffer + offset, buffer_size - offset, "}");
}

static PtxTensorResidency *ptx_tensor_residency_find(PtxFn *fn,
                                                      uint32_t id) {
  if (!fn || id == 0) return NULL;
  for (size_t i = 0; i < fn->tensor_residency_count; i++) {
    if (fn->tensor_residencies[i].id == id)
      return &fn->tensor_residencies[i];
  }
  return NULL;
}

static PtxTensorResidency *ptx_tensor_residency_add(PtxFn *fn,
                                                     uint32_t id) {
  if (!fn || id == 0 || ptx_tensor_residency_find(fn, id)) return NULL;
  if (fn->tensor_residency_count == fn->tensor_residency_capacity) {
    size_t capacity = fn->tensor_residency_capacity
                          ? fn->tensor_residency_capacity * 2
                          : 4;
    PtxTensorResidency *grown = realloc(
        fn->tensor_residencies, capacity * sizeof(*grown));
    if (!grown) return NULL;
    fn->tensor_residencies = grown;
    fn->tensor_residency_capacity = capacity;
  }
  PtxTensorResidency *group =
      &fn->tensor_residencies[fn->tensor_residency_count++];
  memset(group, 0, sizeof(*group));
  group->id = id;
  return group;
}

static const char *ptx_tensor_residency_name(
    IRTensorResidencyScope scope) {
  switch (scope) {
  case IR_TENSOR_RESIDENCY_SCOPE_LOOP:
    return "tensor_loop";
  case IR_TENSOR_RESIDENCY_SCOPE_PIPELINE:
    return "tensor_pipeline";
  case IR_TENSOR_RESIDENCY_SCOPE_REGION:
    return "tensor_region";
  default:
    return NULL;
  }
}

static const char *ptx_wmma_space(PtxVal pointer) {
  switch (pointer.address_space) {
  case MTLC_ADDRESS_SPACE_DEFAULT:
  case MTLC_ADDRESS_SPACE_GENERIC:
    return "";
  case MTLC_ADDRESS_SPACE_GLOBAL:
    return ".global";
  case MTLC_ADDRESS_SPACE_WORKGROUP:
    return ".shared";
  default:
    return NULL;
  }
}

typedef struct {
  unsigned group_coefficient;
  unsigned thread_coefficient;
  unsigned constant;
} PtxMmaCoordinate;

typedef struct {
  char bases[6][24];
  const char *spaces[6];
  char strides[4][24];
  char scale_strides[2][24];
  char c_scale_base[24];
  const char *c_scale_space;
  char metadata_stride[24];
  int dense_contiguous[2];
  int c_zero;
} PtxMmaTileMemory;

typedef struct {
  int a_base;
  int metadata_register;
} PtxMmaSparseAFragment;

static int ptx_tensor_stride_registers(PtxFn *fn, const IRInstruction *in,
                                       size_t base, size_t per_tile,
                                       char registers[4][24]);

static void ptx_mma_emit_coordinate(PtxFn *fn, PtxMmaCoordinate coordinate,
                                    const char *group, const char *thread,
                                    char result[24]) {
  reg_name(PC_B32, new_reg(fn, PC_B32), result);
  sb_printf(&fn->body, "\tmov.u32 %s, %u;\n", result, coordinate.constant);
  if (coordinate.group_coefficient == 1) {
    sb_printf(&fn->body, "\tadd.u32 %s, %s, %s;\n", result, result, group);
  } else if (coordinate.group_coefficient > 1) {
    sb_printf(&fn->body, "\tmad.lo.u32 %s, %s, %u, %s;\n", result, group,
              coordinate.group_coefficient, result);
  }
  if (coordinate.thread_coefficient == 1) {
    sb_printf(&fn->body, "\tadd.u32 %s, %s, %s;\n", result, result, thread);
  } else if (coordinate.thread_coefficient > 1) {
    sb_printf(&fn->body, "\tmad.lo.u32 %s, %s, %u, %s;\n", result, thread,
              coordinate.thread_coefficient, result);
  }
}

static void ptx_mma_emit_address(PtxFn *fn, const char *base,
                                 const char *leading_dimension,
                                 MtlcTensorLayout layout, int transpose,
                                 PtxMmaCoordinate logical_row,
                                 PtxMmaCoordinate logical_column,
                                 unsigned element_bytes, const char *group,
                                 const char *thread, char address[24]) {
  PtxMmaCoordinate storage_row = transpose ? logical_column : logical_row;
  PtxMmaCoordinate storage_column = transpose ? logical_row : logical_column;
  char row[24], column[24], linear[24], byte_offset[24];
  ptx_mma_emit_coordinate(fn, storage_row, group, thread, row);
  ptx_mma_emit_coordinate(fn, storage_column, group, thread, column);
  reg_name(PC_B32, new_reg(fn, PC_B32), linear);
  if (layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR) {
    sb_printf(&fn->body, "\tmul.lo.u32 %s, %s, %s;\n", linear, row,
              leading_dimension);
    sb_printf(&fn->body, "\tadd.u32 %s, %s, %s;\n", linear, linear,
              column);
  } else {
    sb_printf(&fn->body, "\tmul.lo.u32 %s, %s, %s;\n", linear, column,
              leading_dimension);
    sb_printf(&fn->body, "\tadd.u32 %s, %s, %s;\n", linear, linear, row);
  }
  reg_name(PC_B64, new_reg(fn, PC_B64), byte_offset);
  sb_printf(&fn->body, "\tmul.wide.u32 %s, %s, %u;\n", byte_offset,
            linear, element_bytes);
  reg_name(PC_B64, new_reg(fn, PC_B64), address);
  sb_printf(&fn->body, "\tadd.u64 %s, %s, %s;\n", address, base,
            byte_offset);
}

static void ptx_mma_emit_nibble_address(
    PtxFn *fn, const char *base, const char *leading_dimension,
    MtlcTensorLayout layout, int transpose, PtxMmaCoordinate logical_row,
    PtxMmaCoordinate logical_column, const char *group, const char *thread,
    char address[24], char linear_out[24]) {
  PtxMmaCoordinate storage_row = transpose ? logical_column : logical_row;
  PtxMmaCoordinate storage_column = transpose ? logical_row : logical_column;
  char row[24], column[24], byte_index[24], byte_offset[24];
  ptx_mma_emit_coordinate(fn, storage_row, group, thread, row);
  ptx_mma_emit_coordinate(fn, storage_column, group, thread, column);
  reg_name(PC_B32, new_reg(fn, PC_B32), linear_out);
  if (layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR) {
    sb_printf(&fn->body, "\tmul.lo.u32 %s, %s, %s;\n", linear_out, row,
              leading_dimension);
    sb_printf(&fn->body, "\tadd.u32 %s, %s, %s;\n", linear_out,
              linear_out, column);
  } else {
    sb_printf(&fn->body, "\tmul.lo.u32 %s, %s, %s;\n", linear_out, column,
              leading_dimension);
    sb_printf(&fn->body, "\tadd.u32 %s, %s, %s;\n", linear_out,
              linear_out, row);
  }
  reg_name(PC_B32, new_reg(fn, PC_B32), byte_index);
  sb_printf(&fn->body, "\tshr.u32 %s, %s, 1;\n", byte_index, linear_out);
  reg_name(PC_B64, new_reg(fn, PC_B64), byte_offset);
  sb_printf(&fn->body, "\tcvt.u64.u32 %s, %s;\n", byte_offset, byte_index);
  reg_name(PC_B64, new_reg(fn, PC_B64), address);
  sb_printf(&fn->body, "\tadd.u64 %s, %s, %s;\n", address, base,
            byte_offset);
}

static void ptx_mma_load_packed_nibbles(
    PtxFn *fn, const char *base, const char *leading_dimension,
    MtlcTensorLayout layout, int transpose, const char *space,
    const PtxMmaCoordinate rows[8], const PtxMmaCoordinate columns[8],
    const char *group, const char *thread, int destination_register,
    int contiguous_word) {
  char destination[24];
  reg_name(PC_B32, destination_register, destination);
  if (contiguous_word) {
    char address[24], linear[24];
    ptx_mma_emit_nibble_address(fn, base, leading_dimension, layout,
                                transpose, rows[0], columns[0], group,
                                thread, address, linear);
    sb_printf(&fn->body, "\tld%s.b32 %s, [%s];\n", space, destination,
              address);
    return;
  }
  for (unsigned nibble = 0; nibble < 8; nibble++) {
    char address[24], linear[24], packed_byte[24], shift[24], value[24];
    ptx_mma_emit_nibble_address(fn, base, leading_dimension, layout,
                                transpose, rows[nibble], columns[nibble],
                                group, thread, address, linear);
    reg_name(PC_B32, new_reg(fn, PC_B32), packed_byte);
    sb_printf(&fn->body, "\tld%s.u8 %s, [%s];\n", space, packed_byte,
              address);
    reg_name(PC_B32, new_reg(fn, PC_B32), shift);
    sb_printf(&fn->body, "\tand.b32 %s, %s, 1;\n", shift, linear);
    sb_printf(&fn->body, "\tshl.b32 %s, %s, 2;\n", shift, shift);
    reg_name(PC_B32, new_reg(fn, PC_B32), value);
    sb_printf(&fn->body, "\tshr.u32 %s, %s, %s;\n", value, packed_byte,
              shift);
    sb_printf(&fn->body, "\tand.b32 %s, %s, 15;\n", value, value);
    if (nibble == 0) {
      sb_printf(&fn->body, "\tmov.b32 %s, %s;\n", destination, value);
    } else {
      sb_printf(&fn->body, "\tshl.b32 %s, %s, %u;\n", value, value,
                nibble * 4);
      sb_printf(&fn->body, "\tor.b32 %s, %s, %s;\n", destination,
                destination, value);
    }
  }
}

static void ptx_mma_load_u8_pair(
    PtxFn *fn, const char *base, const char *leading_dimension,
    MtlcTensorLayout layout, const char *space,
    const PtxMmaCoordinate rows[2], const PtxMmaCoordinate columns[2],
    const char *group, const char *thread, int destination_register) {
  char destination[24];
  reg_name(PC_B32, destination_register, destination);
  for (unsigned byte = 0; byte < 2; byte++) {
    char address[24], value[24];
    ptx_mma_emit_address(fn, base, leading_dimension, layout, 0, rows[byte],
                         columns[byte], 1, group, thread, address);
    reg_name(PC_B32, new_reg(fn, PC_B32), value);
    sb_printf(&fn->body, "\tld%s.u8 %s, [%s];\n", space, value, address);
    if (byte == 0) {
      sb_printf(&fn->body, "\tmov.b32 %s, %s;\n", destination, value);
    } else {
      sb_printf(&fn->body, "\tshl.b32 %s, %s, 8;\n", value, value);
      sb_printf(&fn->body, "\tor.b32 %s, %s, %s;\n", destination,
                destination, value);
    }
  }
}

static void ptx_mma_load_u16_pair(
    PtxFn *fn, const char *base, const char *leading_dimension,
    MtlcTensorLayout layout, int transpose, const char *space,
    const PtxMmaCoordinate rows[2], const PtxMmaCoordinate columns[2],
    const char *group, const char *thread, int destination_register) {
  char destination[24];
  reg_name(PC_B32, destination_register, destination);
  for (unsigned half = 0; half < 2; half++) {
    char address[24], value[24];
    ptx_mma_emit_address(fn, base, leading_dimension, layout, transpose,
                         rows[half], columns[half], 2, group, thread, address);
    reg_name(PC_B32, new_reg(fn, PC_B32), value);
    sb_printf(&fn->body, "\tld%s.u16 %s, [%s];\n", space, value, address);
    if (half == 0) {
      sb_printf(&fn->body, "\tmov.b32 %s, %s;\n", destination, value);
    } else {
      sb_printf(&fn->body, "\tshl.b32 %s, %s, 16;\n", value, value);
      sb_printf(&fn->body, "\tor.b32 %s, %s, %s;\n", destination,
                destination, value);
    }
  }
}

static void ptx_mma_load_packed_bytes(
    PtxFn *fn, const char *base, const char *leading_dimension,
    MtlcTensorLayout layout, int transpose, const char *space,
    const PtxMmaCoordinate rows[4], const PtxMmaCoordinate columns[4],
    const char *group, const char *thread, int destination_register) {
  char destination[24];
  reg_name(PC_B32, destination_register, destination);
  for (unsigned byte = 0; byte < 4; byte++) {
    char address[24], value[24];
    ptx_mma_emit_address(fn, base, leading_dimension, layout, transpose,
                         rows[byte], columns[byte], 1, group, thread, address);
    reg_name(PC_B32, new_reg(fn, PC_B32), value);
    sb_printf(&fn->body, "\tld%s.u8 %s, [%s];\n", space, value, address);
    if (byte == 0) {
      sb_printf(&fn->body, "\tmov.b32 %s, %s;\n", destination, value);
    } else {
      char shifted[24];
      reg_name(PC_B32, new_reg(fn, PC_B32), shifted);
      sb_printf(&fn->body, "\tshl.b32 %s, %s, %u;\n", shifted, value,
                byte * 8);
      sb_printf(&fn->body, "\tor.b32 %s, %s, %s;\n", destination,
                destination, shifted);
    }
  }
}

static void ptx_mma_load_dense_subbytes(
    PtxFn *fn, const char *base, const char *leading_dimension,
    MtlcTensorLayout layout, int transpose, const char *space,
    const PtxMmaCoordinate rows[4], const PtxMmaCoordinate columns[4],
    const char *group, const char *thread, unsigned bits,
    int destination_register) {
  char destination[24];
  reg_name(PC_B32, destination_register, destination);
  for (unsigned container = 0; container < 4; container++) {
    PtxMmaCoordinate storage_row = transpose ? columns[container]
                                             : rows[container];
    PtxMmaCoordinate storage_column = transpose ? rows[container]
                                                : columns[container];
    char row[24], column[24], linear[24], bit_index[24], byte_index[24];
    char bit_shift[24], byte_offset[24], address[24], next_address[24];
    char low[24], high[24], word[24], value[24], crosses[24];
    ptx_mma_emit_coordinate(fn, storage_row, group, thread, row);
    ptx_mma_emit_coordinate(fn, storage_column, group, thread, column);
    reg_name(PC_B32, new_reg(fn, PC_B32), linear);
    if (layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR) {
      sb_printf(&fn->body, "\tmul.lo.u32 %s, %s, %s;\n", linear, row,
                leading_dimension);
      sb_printf(&fn->body, "\tadd.u32 %s, %s, %s;\n", linear, linear,
                column);
    } else {
      sb_printf(&fn->body, "\tmul.lo.u32 %s, %s, %s;\n", linear, column,
                leading_dimension);
      sb_printf(&fn->body, "\tadd.u32 %s, %s, %s;\n", linear, linear, row);
    }
    reg_name(PC_B32, new_reg(fn, PC_B32), bit_index);
    sb_printf(&fn->body, "\tmul.lo.u32 %s, %s, %u;\n", bit_index, linear,
              bits);
    reg_name(PC_B32, new_reg(fn, PC_B32), byte_index);
    sb_printf(&fn->body, "\tshr.u32 %s, %s, 3;\n", byte_index, bit_index);
    reg_name(PC_B32, new_reg(fn, PC_B32), bit_shift);
    sb_printf(&fn->body, "\tand.b32 %s, %s, 7;\n", bit_shift, bit_index);
    reg_name(PC_B64, new_reg(fn, PC_B64), byte_offset);
    sb_printf(&fn->body, "\tcvt.u64.u32 %s, %s;\n", byte_offset,
              byte_index);
    reg_name(PC_B64, new_reg(fn, PC_B64), address);
    sb_printf(&fn->body, "\tadd.u64 %s, %s, %s;\n", address, base,
              byte_offset);
    reg_name(PC_B32, new_reg(fn, PC_B32), low);
    sb_printf(&fn->body, "\tld%s.u8 %s, [%s];\n", space, low, address);
    reg_name(PC_B32, new_reg(fn, PC_B32), high);
    sb_printf(&fn->body, "\tmov.u32 %s, 0;\n", high);
    reg_name(PC_PRED, new_reg(fn, PC_PRED), crosses);
    sb_printf(&fn->body, "\tsetp.gt.u32 %s, %s, %u;\n", crosses,
              bit_shift, 8u - bits);
    reg_name(PC_B64, new_reg(fn, PC_B64), next_address);
    sb_printf(&fn->body, "\tadd.u64 %s, %s, 1;\n", next_address,
              address);
    sb_printf(&fn->body, "\t@%s ld%s.u8 %s, [%s];\n", crosses, space,
              high, next_address);
    reg_name(PC_B32, new_reg(fn, PC_B32), word);
    sb_printf(&fn->body, "\tshl.b32 %s, %s, 8;\n", word, high);
    sb_printf(&fn->body, "\tor.b32 %s, %s, %s;\n", word, word, low);
    reg_name(PC_B32, new_reg(fn, PC_B32), value);
    sb_printf(&fn->body, "\tshr.u32 %s, %s, %s;\n", value, word,
              bit_shift);
    sb_printf(&fn->body, "\tand.b32 %s, %s, %u;\n", value, value,
              (1u << bits) - 1u);
    if (container == 0) {
      sb_printf(&fn->body, "\tmov.b32 %s, %s;\n", destination, value);
    } else {
      sb_printf(&fn->body, "\tshl.b32 %s, %s, %u;\n", value, value,
                container * 8);
      sb_printf(&fn->body, "\tor.b32 %s, %s, %s;\n", destination,
                destination, value);
    }
  }
}

static void ptx_mma_load_contiguous_dense_subbytes(
    PtxFn *fn, const char *base, const char *leading_dimension,
    MtlcTensorLayout layout, const char *space, PtxMmaCoordinate row,
    PtxMmaCoordinate column, const char *group, const char *thread,
    unsigned bits, int destination_register) {
  PtxMmaCoordinate storage_row = row;
  PtxMmaCoordinate storage_column = column;
  char row_register[24], column_register[24], linear[24], bit_index[24];
  char byte_index[24], byte_offset[24], address[24], packed[24];
  ptx_mma_emit_coordinate(fn, storage_row, group, thread, row_register);
  ptx_mma_emit_coordinate(fn, storage_column, group, thread, column_register);
  reg_name(PC_B32, new_reg(fn, PC_B32), linear);
  if (layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR) {
    sb_printf(&fn->body, "\tmul.lo.u32 %s, %s, %s;\n", linear,
              row_register, leading_dimension);
    sb_printf(&fn->body, "\tadd.u32 %s, %s, %s;\n", linear, linear,
              column_register);
  } else {
    sb_printf(&fn->body, "\tmul.lo.u32 %s, %s, %s;\n", linear,
              column_register, leading_dimension);
    sb_printf(&fn->body, "\tadd.u32 %s, %s, %s;\n", linear, linear,
              row_register);
  }
  reg_name(PC_B32, new_reg(fn, PC_B32), bit_index);
  sb_printf(&fn->body, "\tmul.lo.u32 %s, %s, %u;\n", bit_index, linear,
            bits);
  reg_name(PC_B32, new_reg(fn, PC_B32), byte_index);
  sb_printf(&fn->body, "\tshr.u32 %s, %s, 3;\n", byte_index, bit_index);
  reg_name(PC_B64, new_reg(fn, PC_B64), byte_offset);
  sb_printf(&fn->body, "\tcvt.u64.u32 %s, %s;\n", byte_offset, byte_index);
  reg_name(PC_B64, new_reg(fn, PC_B64), address);
  sb_printf(&fn->body, "\tadd.u64 %s, %s, %s;\n", address, base,
            byte_offset);
  reg_name(PC_B32, new_reg(fn, PC_B32), packed);
  sb_printf(&fn->body, "\tmov.u32 %s, 0;\n", packed);
  unsigned byte_count = (4u * bits + 7u) / 8u;
  for (unsigned byte = 0; byte < byte_count; byte++) {
    char value[24];
    reg_name(PC_B32, new_reg(fn, PC_B32), value);
    if (byte == 0)
      sb_printf(&fn->body, "\tld%s.u8 %s, [%s];\n", space, value,
                address);
    else
      sb_printf(&fn->body, "\tld%s.u8 %s, [%s+%u];\n", space, value,
                address, byte);
    if (byte != 0)
      sb_printf(&fn->body, "\tshl.b32 %s, %s, %u;\n", value, value,
                byte * 8);
    sb_printf(&fn->body, "\tor.b32 %s, %s, %s;\n", packed, packed, value);
  }

  char destination[24];
  reg_name(PC_B32, destination_register, destination);
  for (unsigned container = 0; container < 4; container++) {
    char value[24];
    reg_name(PC_B32, new_reg(fn, PC_B32), value);
    sb_printf(&fn->body, "\tbfe.u32 %s, %s, %u, %u;\n", value, packed,
              container * bits, bits);
    if (container == 0) {
      sb_printf(&fn->body, "\tmov.b32 %s, %s;\n", destination, value);
    } else {
      sb_printf(&fn->body, "\tshl.b32 %s, %s, %u;\n", value, value,
                container * 8);
      sb_printf(&fn->body, "\tor.b32 %s, %s, %s;\n", destination,
                destination, value);
    }
  }
}

static void ptx_mma_load_narrow_containers(
    PtxFn *fn, const char *base, const char *leading_dimension,
    MtlcTensorLayout layout, int transpose, MtlcTensorPacking packing,
    const char *space, const PtxMmaCoordinate rows[4],
    const PtxMmaCoordinate columns[4], const char *group, const char *thread,
    unsigned bits, int destination_register, int contiguous) {
  if (packing == MTLC_TENSOR_PACKING_DENSE_SUBBYTE && contiguous) {
    ptx_mma_load_contiguous_dense_subbytes(
        fn, base, leading_dimension, layout, space, rows[0], columns[0], group,
        thread, bits, destination_register);
  } else if (packing == MTLC_TENSOR_PACKING_DENSE_SUBBYTE) {
    ptx_mma_load_dense_subbytes(fn, base, leading_dimension, layout,
                                transpose, space, rows, columns, group,
                                thread, bits, destination_register);
  } else {
    ptx_mma_load_packed_bytes(fn, base, leading_dimension, layout, transpose,
                              space, rows, columns, group, thread,
                              destination_register);
  }
}

static void ptx_mma_load_u8(PtxFn *fn, const char *base,
                            const char *leading_dimension,
                            MtlcTensorLayout layout, const char *space,
                            PtxMmaCoordinate row, PtxMmaCoordinate column,
                            const char *group, const char *thread,
                            int destination_register) {
  char address[24], destination[24];
  ptx_mma_emit_address(fn, base, leading_dimension, layout, 0, row, column, 1,
                       group, thread, address);
  reg_name(PC_B32, destination_register, destination);
  sb_printf(&fn->body, "\tld%s.u8 %s, [%s];\n", space, destination,
            address);
}

static void ptx_mma_load_f32(PtxFn *fn, const char *base,
                             const char *leading_dimension,
                             MtlcTensorLayout layout, const char *space,
                             PtxMmaCoordinate row,
                             PtxMmaCoordinate column, const char *group,
                             const char *thread, int destination_register) {
  char address[24], destination[24];
  ptx_mma_emit_address(fn, base, leading_dimension, layout, 0, row, column, 4,
                       group, thread, address);
  reg_name(PC_F32, destination_register, destination);
  sb_printf(&fn->body, "\tld%s.f32 %s, [%s];\n", space, destination,
            address);
}

static void ptx_mma_load_c(PtxFn *fn, const PtxMmaTileMemory *memory,
                           MtlcTensorLayout layout, PtxMmaCoordinate row,
                           PtxMmaCoordinate column, const char *group,
                           const char *thread, int destination_register) {
  if (memory->c_zero) {
    char destination[24];
    reg_name(PC_F32, destination_register, destination);
    sb_printf(&fn->body, "\tmov.f32 %s, 0f00000000;\n", destination);
    return;
  }
  ptx_mma_load_f32(fn, memory->bases[2], memory->strides[2], layout,
                   memory->spaces[2], row, column, group, thread,
                   destination_register);
}

static void ptx_mma_store_f32(PtxFn *fn, const char *base,
                              const char *leading_dimension,
                              MtlcTensorLayout layout, const char *space,
                              PtxMmaCoordinate row,
                              PtxMmaCoordinate column, const char *group,
                              const char *thread, int source_register) {
  char address[24], source[24];
  ptx_mma_emit_address(fn, base, leading_dimension, layout, 0, row, column, 4,
                       group, thread, address);
  reg_name(PC_F32, source_register, source);
  sb_printf(&fn->body, "\tst%s.f32 [%s], %s;\n", space, address, source);
}

static void ptx_mma_store_f32_accumulator_subtile(
    PtxFn *fn, const IRInstruction *in, const PtxMmaProfile *profile,
    const char *dp, const char *dspace, const char *d_stride,
    const char *group, const char *thread, unsigned m_offset,
    unsigned n_offset, int accumulator_base) {
  for (int element = 0; element < profile->accumulator_registers;
       element++) {
    PtxMmaCoordinate row = {
        1, 0, m_offset + (element >= 2 ? 8u : 0u)};
    PtxMmaCoordinate column = {
        0, 2, n_offset + (unsigned)(element & 1)};
    ptx_mma_store_f32(fn, dp, d_stride, IR_TENSOR_MMA(in).d_layout, dspace, row,
                      column, group, thread, accumulator_base + element);
  }
}

static const char *ptx_mma_kind_name(const PtxMmaProfile *profile) {
  if (!profile) return "unknown";
  if (profile->kind == PTX_MMA_SPARSE_F16) return "sparse-f16-2to4";
  if (profile->kind == PTX_MMA_SPARSE_BF16) return "sparse-bf16-2to4";
  if (profile->kind == PTX_MMA_MXF8F6F4) return "mxf8f6f4";
  if (profile->kind == PTX_MMA_MXFP4) return "mxfp4";
  if (profile->kind == PTX_MMA_NVFP4) return "nvfp4";
  if (profile->kind == PTX_MMA_S8_SCALED) return "s8-block-scaled";
  if (profile->kind == PTX_MMA_F16) {
    int bf16 = strcmp(profile->a_type, "bf16") == 0;
    if (!profile->c_row_scaled) return bf16 ? "bf16" : "f16";
    return bf16 ? "bf16-c-row-scaled" : "f16-c-row-scaled";
  }
  return "fp8";
}

static int ptx_mma_profile_is_sparse(const PtxMmaProfile *profile) {
  return profile &&
         (profile->kind == PTX_MMA_SPARSE_F16 ||
          profile->kind == PTX_MMA_SPARSE_BF16);
}

static int ptx_tensor_tuple_budget(const PtxFn *fn) {
  if (fn && fn->tensor_tuple_budget > 0) return fn->tensor_tuple_budget;
  return fn && fn->target_arch >= 90 ? 96 : 64;
}

static int ptx_mma_prepare_tile_memory(PtxFn *fn,
                                       const IRInstruction *in,
                                       const PtxMmaProfile *profile,
                                       size_t base, size_t per_tile,
                                       PtxMmaTileMemory *memory) {
  memset(memory, 0, sizeof(*memory));
  for (size_t pointer = 0; pointer < 4; pointer++) {
    if (pointer == 2 && ir_tensor_c_is_zero(&in->arguments[base + pointer])) {
      memory->c_zero = 1;
      continue;
    }
    PtxVal value = operand_desc(fn, &in->arguments[base + pointer]);
    memory->spaces[pointer] = ptx_wmma_space(value);
    if (!value.is_ptr || !memory->spaces[pointer]) return 0;
    use_as(fn, &in->arguments[base + pointer], PC_B64,
           memory->bases[pointer]);
  }
  if (ptx_mma_profile_is_sparse(profile)) {
    PtxVal value = operand_desc(fn, &in->arguments[base + 4]);
    memory->spaces[4] = ptx_wmma_space(value);
    if (!value.is_ptr || !memory->spaces[4]) return 0;
    use_as(fn, &in->arguments[base + 4], PC_B64, memory->bases[4]);
    reg_name(PC_B32, new_reg(fn, PC_B32), memory->metadata_stride);
    sb_printf(&fn->body, "\tmov.u32 %s, %u;\n", memory->metadata_stride,
              (unsigned)IR_TENSOR_MMA(in).k / 4u);
  } else if (profile->kind == PTX_MMA_MXF8F6F4 ||
             profile->kind == PTX_MMA_MXFP4 ||
             profile->kind == PTX_MMA_NVFP4 ||
             profile->kind == PTX_MMA_S8_SCALED) {
    for (size_t scale = 0; scale < 2; scale++) {
      size_t argument = base + 4 + scale;
      PtxVal value = operand_desc(fn, &in->arguments[argument]);
      memory->spaces[4 + scale] = ptx_wmma_space(value);
      if (!value.is_ptr || !memory->spaces[4 + scale]) return 0;
      use_as(fn, &in->arguments[argument], PC_B64,
             memory->bases[4 + scale]);
      uint32_t leading_dimension =
          scale == 0 ? IR_TENSOR_MMA(in).a_scale_leading_dimension
                     : IR_TENSOR_MMA(in).b_scale_leading_dimension;
      if (!leading_dimension)
        leading_dimension = (uint32_t)profile->scale_vectors;
      reg_name(PC_B32, new_reg(fn, PC_B32), memory->scale_strides[scale]);
      sb_printf(&fn->body, "\tmov.u32 %s, %u;\n",
                memory->scale_strides[scale], leading_dimension);
    }
  }
  if (!ptx_tensor_stride_registers(fn, in, base, per_tile,
                                   memory->strides))
    return 0;
  if (IR_TENSOR_MMA(in).c_scale_mode != MTLC_TENSOR_SCALE_NONE) {
    size_t argument = base + per_tile - 1u;
    PtxVal value = operand_desc(fn, &in->arguments[argument]);
    memory->c_scale_space = ptx_wmma_space(value);
    if (!value.is_ptr || !memory->c_scale_space) return 0;
    use_as(fn, &in->arguments[argument], PC_B64, memory->c_scale_base);
  }
  memory->dense_contiguous[0] =
      profile->a_bits < 8 &&
      IR_TENSOR_MMA(in).a_packing == MTLC_TENSOR_PACKING_DENSE_SUBBYTE &&
      IR_TENSOR_MMA(in).a_layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR &&
      !IR_TENSOR_MMA(in).transpose_a &&
      IR_TENSOR_MMA(in).a_leading_dimension != 0 &&
      ((IR_TENSOR_MMA(in).a_leading_dimension *
        (uint32_t)profile->a_bits) & 7u) == 0;
  memory->dense_contiguous[1] =
      profile->b_bits < 8 &&
      IR_TENSOR_MMA(in).b_packing == MTLC_TENSOR_PACKING_DENSE_SUBBYTE &&
      IR_TENSOR_MMA(in).b_layout == MTLC_TENSOR_LAYOUT_COLUMN_MAJOR &&
      !IR_TENSOR_MMA(in).transpose_b &&
      IR_TENSOR_MMA(in).b_leading_dimension != 0 &&
      ((IR_TENSOR_MMA(in).b_leading_dimension *
        (uint32_t)profile->b_bits) & 7u) == 0;
  return 1;
}

static void ptx_emit_mma_byte_subtile(
    PtxFn *fn, const IRInstruction *in, const PtxMmaProfile *profile,
    const PtxMmaTileMemory *memory, const char *group, const char *thread,
    unsigned m_offset, unsigned n_offset,
    int accumulator_base, int load_accumulator, int store_accumulator) {
  int a_base = fn->count[PC_B32];
  for (int i = 0; i < profile->a_registers; i++) new_reg(fn, PC_B32);
  int b_base = fn->count[PC_B32];
  for (int i = 0; i < profile->b_registers; i++) new_reg(fn, PC_B32);
  int scale_a = -1, scale_b = -1;
  if (profile->kind == PTX_MMA_MXF8F6F4) {
    scale_a = new_reg(fn, PC_B32);
    scale_b = new_reg(fn, PC_B32);
  }
  if (accumulator_base < 0) {
    accumulator_base = fn->count[PC_F32];
    for (int i = 0; i < profile->accumulator_registers; i++)
      new_reg(fn, PC_F32);
  }

  int direct_a = memory->dense_contiguous[0];
  int direct_b = memory->dense_contiguous[1];

  for (int reg = 0; reg < profile->a_registers; reg++) {
    PtxMmaCoordinate rows[4], columns[4];
    for (int byte = 0; byte < 4; byte++) {
      int element = reg * 4 + byte;
      unsigned second_row = profile->a_registers == 2
                                ? (element >= 4 ? 8u : 0u)
                                : ((element & 7) >= 4 ? 8u : 0u);
      unsigned second_k = profile->a_registers == 4 && element >= 8 ? 16u : 0u;
      rows[byte] = (PtxMmaCoordinate){1, 0, m_offset + second_row};
      columns[byte] =
          (PtxMmaCoordinate){0, 4, second_k + (unsigned)(element & 3)};
    }
    ptx_mma_load_narrow_containers(
        fn, memory->bases[0], memory->strides[0],
        IR_TENSOR_MMA(in).a_layout, IR_TENSOR_MMA(in).transpose_a,
        IR_TENSOR_MMA(in).a_packing, memory->spaces[0], rows, columns, group,
        thread, (unsigned)profile->a_bits, a_base + reg, direct_a);
  }
  for (int reg = 0; reg < profile->b_registers; reg++) {
    PtxMmaCoordinate rows[4], columns[4];
    for (int byte = 0; byte < 4; byte++) {
      int element = reg * 4 + byte;
      unsigned second_k = profile->b_registers == 2 && element >= 4 ? 16u : 0u;
      rows[byte] =
          (PtxMmaCoordinate){0, 4, second_k + (unsigned)(element & 3)};
      columns[byte] = (PtxMmaCoordinate){1, 0, n_offset};
    }
    ptx_mma_load_narrow_containers(
        fn, memory->bases[1], memory->strides[1],
        IR_TENSOR_MMA(in).b_layout, IR_TENSOR_MMA(in).transpose_b,
        IR_TENSOR_MMA(in).b_packing, memory->spaces[1], rows, columns, group,
        thread, (unsigned)profile->b_bits, b_base + reg, direct_b);
  }
  if (profile->kind == PTX_MMA_MXF8F6F4) {
    char thread_low[24];
    reg_name(PC_B32, new_reg(fn, PC_B32), thread_low);
    sb_printf(&fn->body, "\tand.b32 %s, %s, 1;\n", thread_low, thread);
    PtxMmaCoordinate scale_a_row = {1, 8, m_offset};
    PtxMmaCoordinate scale_a_column = {0, 0, 0};
    PtxMmaCoordinate scale_b_row = {0, 0, 0};
    PtxMmaCoordinate scale_b_column = {1, 0, n_offset};
    ptx_mma_load_u8(fn, memory->bases[4], memory->scale_strides[0],
                    MTLC_TENSOR_LAYOUT_ROW_MAJOR, memory->spaces[4],
                    scale_a_row, scale_a_column, group, thread_low, scale_a);
    ptx_mma_load_u8(fn, memory->bases[5], memory->scale_strides[1],
                    MTLC_TENSOR_LAYOUT_COLUMN_MAJOR, memory->spaces[5],
                    scale_b_row, scale_b_column, group, thread, scale_b);
  }
  if (load_accumulator) {
    for (int element = 0; element < profile->accumulator_registers;
         element++) {
      PtxMmaCoordinate row = {
          1, 0, m_offset + (element >= 2 ? 8u : 0u)};
      PtxMmaCoordinate column = {
          0, 2, n_offset + (unsigned)(element & 1)};
      ptx_mma_load_c(fn, memory, IR_TENSOR_MMA(in).c_layout, row, column,
                     group, thread, accumulator_base + element);
    }
  }

  char a[128], b[128], accumulator[128];
  ptx_reg_tuple_at(PC_B32, a_base, profile->a_registers, a, sizeof(a));
  ptx_reg_tuple_at(PC_B32, b_base, profile->b_registers, b, sizeof(b));
  ptx_reg_tuple_at(PC_F32, accumulator_base,
                   profile->accumulator_registers, accumulator,
                   sizeof(accumulator));
  if (profile->kind == PTX_MMA_MXF8F6F4) {
    char scale_a_register[24], scale_b_register[24];
    reg_name(PC_B32, scale_a, scale_a_register);
    reg_name(PC_B32, scale_b, scale_b_register);
    sb_printf(
        &fn->body,
        "\tmma.sync.aligned.%s.row.col.kind::mxf8f6f4.block_scale.scale_vec::1X.%s.%s.%s.%s.%s %s, %s, %s, %s, %s, {0, 0}, %s, {0, 0};\n",
        profile->shape, profile->d_type, profile->a_type, profile->b_type,
        profile->c_type, profile->scale_type, accumulator, a, b, accumulator,
        scale_a_register, scale_b_register);
  } else {
    sb_printf(&fn->body,
              "\tmma.sync.aligned.%s.row.col.%s.%s.%s.%s %s, %s, %s, %s;\n",
              profile->shape, profile->d_type, profile->a_type,
              profile->b_type, profile->c_type, accumulator, a, b,
              accumulator);
  }

  if (store_accumulator) {
    ptx_mma_store_f32_accumulator_subtile(
        fn, in, profile, memory->bases[3], memory->spaces[3],
        memory->strides[3], group, thread, m_offset, n_offset,
        accumulator_base);
  }
}

static void ptx_emit_mma_fp4_subtile(
    PtxFn *fn, const IRInstruction *in, const PtxMmaProfile *profile,
    const PtxMmaTileMemory *memory, const char *group, const char *thread,
    unsigned m_offset, unsigned n_offset, int accumulator_base,
    int load_accumulator, int store_accumulator) {
  int a_base = fn->count[PC_B32];
  for (int i = 0; i < profile->a_registers; i++) new_reg(fn, PC_B32);
  int b_base = fn->count[PC_B32];
  for (int i = 0; i < profile->b_registers; i++) new_reg(fn, PC_B32);
  int scale_a = new_reg(fn, PC_B32);
  int scale_b = new_reg(fn, PC_B32);
  if (accumulator_base < 0) {
    accumulator_base = fn->count[PC_F32];
    for (int i = 0; i < profile->accumulator_registers; i++)
      new_reg(fn, PC_F32);
  }

  int direct_a = memory->dense_contiguous[0];
  int direct_b = memory->dense_contiguous[1];
  for (int reg = 0; reg < profile->a_registers; reg++) {
    PtxMmaCoordinate rows[8], columns[8];
    for (int nibble = 0; nibble < 8; nibble++) {
      int element = reg * 8 + nibble;
      rows[nibble] = (PtxMmaCoordinate){
          1, 0, m_offset + ((element & 8) ? 8u : 0u)};
      columns[nibble] =
          (PtxMmaCoordinate){0, 8,
                             (element >= 16 ? 32u : 0u) +
                                 (unsigned)(element & 7)};
    }
    ptx_mma_load_packed_nibbles(
        fn, memory->bases[0], memory->strides[0],
        IR_TENSOR_MMA(in).a_layout, IR_TENSOR_MMA(in).transpose_a,
        memory->spaces[0], rows, columns, group, thread, a_base + reg,
        direct_a);
  }
  for (int reg = 0; reg < profile->b_registers; reg++) {
    PtxMmaCoordinate rows[8], columns[8];
    for (int nibble = 0; nibble < 8; nibble++) {
      int element = reg * 8 + nibble;
      rows[nibble] =
          (PtxMmaCoordinate){0, 8,
                             (element >= 8 ? 32u : 0u) +
                                 (unsigned)(element & 7)};
      columns[nibble] = (PtxMmaCoordinate){1, 0, n_offset};
    }
    ptx_mma_load_packed_nibbles(
        fn, memory->bases[1], memory->strides[1],
        IR_TENSOR_MMA(in).b_layout, IR_TENSOR_MMA(in).transpose_b,
        memory->spaces[1], rows, columns, group, thread, b_base + reg,
        direct_b);
  }

  char thread_low[24];
  reg_name(PC_B32, new_reg(fn, PC_B32), thread_low);
  sb_printf(&fn->body, "\tand.b32 %s, %s, 1;\n", thread_low, thread);
  if (profile->kind == PTX_MMA_NVFP4) {
    PtxMmaCoordinate scale_a_rows[4], scale_a_columns[4];
    PtxMmaCoordinate scale_b_rows[4], scale_b_columns[4];
    for (unsigned scale = 0; scale < 4; scale++) {
      scale_a_rows[scale] =
          (PtxMmaCoordinate){1, 8, m_offset};
      scale_a_columns[scale] =
          (PtxMmaCoordinate){0, 0, scale};
      scale_b_rows[scale] =
          (PtxMmaCoordinate){0, 0, scale};
      scale_b_columns[scale] =
          (PtxMmaCoordinate){1, 0, n_offset};
    }
    ptx_mma_load_packed_bytes(
        fn, memory->bases[4], memory->scale_strides[0],
        MTLC_TENSOR_LAYOUT_ROW_MAJOR, 0, memory->spaces[4], scale_a_rows,
        scale_a_columns, group, thread_low, scale_a);
    ptx_mma_load_packed_bytes(
        fn, memory->bases[5], memory->scale_strides[1],
        MTLC_TENSOR_LAYOUT_COLUMN_MAJOR, 0, memory->spaces[5], scale_b_rows,
        scale_b_columns, group, thread, scale_b);
  } else {
    PtxMmaCoordinate scale_a_rows[2] = {
        {1, 8, m_offset}, {1, 8, m_offset}};
    PtxMmaCoordinate scale_a_columns[2] = {{0, 0, 0}, {0, 0, 1}};
    PtxMmaCoordinate scale_b_rows[2] = {{0, 0, 0}, {0, 0, 1}};
    PtxMmaCoordinate scale_b_columns[2] = {
        {1, 0, n_offset}, {1, 0, n_offset}};
    ptx_mma_load_u8_pair(fn, memory->bases[4], memory->scale_strides[0],
                         MTLC_TENSOR_LAYOUT_ROW_MAJOR, memory->spaces[4],
                         scale_a_rows, scale_a_columns, group, thread_low,
                         scale_a);
    ptx_mma_load_u8_pair(fn, memory->bases[5], memory->scale_strides[1],
                         MTLC_TENSOR_LAYOUT_COLUMN_MAJOR, memory->spaces[5],
                         scale_b_rows, scale_b_columns, group, thread,
                         scale_b);
  }

  if (load_accumulator) {
    for (int element = 0; element < profile->accumulator_registers;
         element++) {
      PtxMmaCoordinate row = {
          1, 0, m_offset + (element >= 2 ? 8u : 0u)};
      PtxMmaCoordinate column = {
          0, 2, n_offset + (unsigned)(element & 1)};
      ptx_mma_load_c(fn, memory, IR_TENSOR_MMA(in).c_layout, row, column,
                     group, thread, accumulator_base + element);
    }
  }

  char a[128], b[128], accumulator[128], scale_a_register[24];
  char scale_b_register[24];
  ptx_reg_tuple_at(PC_B32, a_base, profile->a_registers, a, sizeof(a));
  ptx_reg_tuple_at(PC_B32, b_base, profile->b_registers, b, sizeof(b));
  ptx_reg_tuple_at(PC_F32, accumulator_base,
                   profile->accumulator_registers, accumulator,
                   sizeof(accumulator));
  reg_name(PC_B32, scale_a, scale_a_register);
  reg_name(PC_B32, scale_b, scale_b_register);
  const char *mma_kind =
      profile->kind == PTX_MMA_NVFP4 ? "mxf4nvf4" : "mxf4";
  sb_printf(
      &fn->body,
      "\tmma.sync.aligned.%s.row.col.kind::%s.block_scale.scale_vec::%dX.%s.%s.%s.%s.%s %s, %s, %s, %s, %s, {0, 0}, %s, {0, 0};\n",
      profile->shape, mma_kind, profile->scale_vectors, profile->d_type,
      profile->a_type, profile->b_type, profile->c_type,
      profile->scale_type, accumulator, a, b, accumulator, scale_a_register,
      scale_b_register);

  if (store_accumulator) {
    ptx_mma_store_f32_accumulator_subtile(
        fn, in, profile, memory->bases[3], memory->spaces[3],
        memory->strides[3], group, thread, m_offset, n_offset,
        accumulator_base);
  }
}

static void ptx_mma_load_sparse_2_to_4_metadata(
    PtxFn *fn, const PtxMmaTileMemory *memory, const char *group,
    unsigned m_offset, int metadata_register) {
  char metadata[24];
  reg_name(PC_B32, metadata_register, metadata);
  sb_printf(&fn->body, "\tmov.u32 %s, 0;\n", metadata);
  for (unsigned chunk = 0; chunk < 8; chunk++) {
    PtxMmaCoordinate row = {
        1, 0, m_offset + (chunk >= 4 ? 8u : 0u)};
    PtxMmaCoordinate column = {0, 0, chunk & 3u};
    char address[24], mask[24], count[24], scratch[24];
    char first[24], second[24], nibble[24], predicate[24];
    ptx_mma_emit_address(fn, memory->bases[4], memory->metadata_stride,
                         MTLC_TENSOR_LAYOUT_ROW_MAJOR, 0, row, column, 1,
                         group, "0", address);
    reg_name(PC_B32, new_reg(fn, PC_B32), mask);
    reg_name(PC_B32, new_reg(fn, PC_B32), count);
    reg_name(PC_B32, new_reg(fn, PC_B32), scratch);
    reg_name(PC_B32, new_reg(fn, PC_B32), first);
    reg_name(PC_B32, new_reg(fn, PC_B32), second);
    reg_name(PC_B32, new_reg(fn, PC_B32), nibble);
    reg_name(PC_PRED, new_reg(fn, PC_PRED), predicate);
    sb_printf(&fn->body,
              "\tld%s.u8 %s, [%s];\n"
              "\tand.b32 %s, %s, 15;\n"
              "\tpopc.b32 %s, %s;\n"
              "\tsetp.ne.u32 %s, %s, 2;\n"
              "\tselp.u32 %s, 3, %s, %s;\n",
              memory->spaces[4], mask, address, mask, mask, count, mask,
              predicate, count, mask, mask, predicate);

    sb_printf(&fn->body,
              "\tand.b32 %s, %s, 2;\n"
              "\tsetp.ne.u32 %s, %s, 0;\n"
              "\tselp.u32 %s, 1, 2, %s;\n"
              "\tand.b32 %s, %s, 1;\n"
              "\tsetp.ne.u32 %s, %s, 0;\n"
              "\tselp.u32 %s, 0, %s, %s;\n",
              scratch, mask, predicate, scratch, first, predicate, scratch,
              mask, predicate, scratch, first, first, predicate);
    sb_printf(&fn->body,
              "\tand.b32 %s, %s, 4;\n"
              "\tsetp.ne.u32 %s, %s, 0;\n"
              "\tselp.u32 %s, 2, 1, %s;\n"
              "\tand.b32 %s, %s, 8;\n"
              "\tsetp.ne.u32 %s, %s, 0;\n"
              "\tselp.u32 %s, 3, %s, %s;\n"
              "\tshl.b32 %s, %s, 2;\n"
              "\tor.b32 %s, %s, %s;\n",
              scratch, mask, predicate, scratch, second, predicate, scratch,
              mask, predicate, scratch, second, second, predicate, second,
              second, nibble, first, second);
    if (chunk != 0)
      sb_printf(&fn->body, "\tshl.b32 %s, %s, %u;\n", nibble, nibble,
                chunk * 4u);
    sb_printf(&fn->body, "\tor.b32 %s, %s, %s;\n", metadata, metadata,
              nibble);
  }
}

static void ptx_mma_prepare_sparse_f16_a(
    PtxFn *fn, const IRInstruction *in, const PtxMmaTileMemory *memory,
    const char *group, const char *thread, unsigned m_offset,
    PtxMmaSparseAFragment *fragment) {
  fragment->a_base = fn->count[PC_B32];
  for (int reg = 0; reg < 2; reg++) new_reg(fn, PC_B32);
  fragment->metadata_register = new_reg(fn, PC_B32);

  for (int reg = 0; reg < 2; reg++) {
    PtxMmaCoordinate rows[2] = {
        {1, 0, m_offset + (reg ? 8u : 0u)},
        {1, 0, m_offset + (reg ? 8u : 0u)}};
    PtxMmaCoordinate columns[2] = {{0, 2, 0}, {0, 2, 1}};
    ptx_mma_load_u16_pair(
        fn, memory->bases[0], memory->strides[0],
        IR_TENSOR_MMA(in).a_layout, IR_TENSOR_MMA(in).transpose_a,
        memory->spaces[0], rows, columns, group, thread,
        fragment->a_base + reg);
  }
  ptx_mma_load_sparse_2_to_4_metadata(
      fn, memory, group, m_offset, fragment->metadata_register);
}

static void ptx_emit_mma_sparse_f16_subtile(
    PtxFn *fn, const IRInstruction *in, const PtxMmaProfile *profile,
    const PtxMmaTileMemory *memory, const char *group, const char *thread,
    const PtxMmaSparseAFragment *sparse_a, unsigned m_offset,
    unsigned n_offset, int accumulator_base, int load_accumulator,
    int store_accumulator) {
  int b_base = fn->count[PC_B32];
  for (int reg = 0; reg < 2; reg++) new_reg(fn, PC_B32);
  if (accumulator_base < 0) {
    accumulator_base = fn->count[PC_F32];
    for (int element = 0; element < 4; element++) new_reg(fn, PC_F32);
  }

  for (int reg = 0; reg < 2; reg++) {
    PtxMmaCoordinate rows[2] = {
        {0, 2, (unsigned)reg * 8u},
        {0, 2, (unsigned)reg * 8u + 1u}};
    PtxMmaCoordinate columns[2] = {
        {1, 0, n_offset}, {1, 0, n_offset}};
    ptx_mma_load_u16_pair(
        fn, memory->bases[1], memory->strides[1],
        IR_TENSOR_MMA(in).b_layout, IR_TENSOR_MMA(in).transpose_b,
        memory->spaces[1], rows, columns, group, thread, b_base + reg);
  }

  if (load_accumulator) {
    for (int element = 0; element < 4; element++) {
      PtxMmaCoordinate row = {
          1, 0, m_offset + (element >= 2 ? 8u : 0u)};
      PtxMmaCoordinate column = {
          0, 2, n_offset + (unsigned)(element & 1)};
      ptx_mma_load_c(fn, memory, IR_TENSOR_MMA(in).c_layout, row, column,
                     group, thread, accumulator_base + element);
    }
  }

  char a[64], b[64], accumulator[128], metadata[24];
  ptx_reg_tuple_at(PC_B32, sparse_a->a_base, 2, a, sizeof(a));
  ptx_reg_tuple_at(PC_B32, b_base, 2, b, sizeof(b));
  ptx_reg_tuple_at(PC_F32, accumulator_base, 4, accumulator,
                   sizeof(accumulator));
  reg_name(PC_B32, sparse_a->metadata_register, metadata);
  const char *sparse_variant =
      ptx_version_at_least(fn, 8, 5) ? "sp::ordered_metadata" : "sp";
  sb_printf(
      &fn->body,
      "\tmma.%s.sync.aligned.%s.row.col.f32.%s.%s.f32 %s, %s, %s, %s, %s, 0;\n",
      sparse_variant, profile->shape, profile->a_type, profile->b_type,
      accumulator, a, b, accumulator, metadata);

  if (store_accumulator) {
    ptx_mma_store_f32_accumulator_subtile(
        fn, in, profile, memory->bases[3], memory->spaces[3],
        memory->strides[3], group, thread, m_offset, n_offset,
        accumulator_base);
  }
}

// Fragment addressing for the block-scaled i8 tile. A lane pointer is
// base + index * coefficient * stride + bytes * byte_coefficient for the
// lane's group or thread, computed once per tile; every fragment or scale
// the lane loads is then that pointer plus a constant -- an immediate
// offset when the leading dimension is static. Shared-memory pointers
// narrow to 32 bits.
typedef struct {
  char reg[24];
  int narrow;
  int stride_known;
  unsigned stride_bytes;
  char stride_reg[24];
} PtxLanePointer;

static int ptx_parse_u32(const char *text, unsigned *out) {
  if (!text || *text < '0' || *text > '9') return 0;
  char *end = NULL;
  unsigned long long value = strtoull(text, &end, 10);
  if (!end || *end || value > 0xFFFFFFFFull) return 0;
  *out = (unsigned)value;
  return 1;
}

// A leading dimension as ptx_lane_pointer wants it: the descriptor's
// static value as a decimal immediate, else the runtime register.
static const char *ptx_static_ld(uint32_t static_value, const char *reg,
                                 char out[24]) {
  if (static_value == 0) return reg;
  snprintf(out, 24, "%u", static_value);
  return out;
}

static void ptx_lane_pointer(PtxFn *fn, const char *base, const char *space,
                             const char *leading_dimension,
                             unsigned element_bytes, const char *index,
                             unsigned index_coefficient, const char *bytes,
                             unsigned byte_coefficient,
                             PtxLanePointer *out) {
  memset(out, 0, sizeof(*out));
  out->narrow = space && strcmp(space, ".shared") == 0;
  unsigned ld = 0;
  out->stride_known = ptx_parse_u32(leading_dimension, &ld);
  char offset[24];
  reg_name(PC_B32, new_reg(fn, PC_B32), offset);
  if (out->stride_known) {
    out->stride_bytes = ld * element_bytes;
    sb_printf(&fn->body, "\tmul.lo.u32 %s, %s, %u;\n", offset, index,
              index_coefficient * out->stride_bytes);
  } else {
    reg_name(PC_B32, new_reg(fn, PC_B32), out->stride_reg);
    sb_printf(&fn->body, "\tmul.lo.u32 %s, %s, %u;\n", out->stride_reg,
              leading_dimension, element_bytes);
    sb_printf(&fn->body, "\tmul.lo.u32 %s, %s, %u;\n", offset, index,
              index_coefficient);
    sb_printf(&fn->body, "\tmul.lo.u32 %s, %s, %s;\n", offset, offset,
              out->stride_reg);
  }
  if (bytes && byte_coefficient)
    sb_printf(&fn->body, "\tmad.lo.u32 %s, %s, %u, %s;\n", offset, bytes,
              byte_coefficient, offset);
  if (out->narrow) {
    char narrow_base[24];
    reg_name(PC_B32, new_reg(fn, PC_B32), narrow_base);
    reg_name(PC_B32, new_reg(fn, PC_B32), out->reg);
    sb_printf(&fn->body, "\tcvt.u32.u64 %s, %s;\n", narrow_base, base);
    sb_printf(&fn->body, "\tadd.u32 %s, %s, %s;\n", out->reg, narrow_base,
              offset);
  } else {
    char wide[24];
    reg_name(PC_B64, new_reg(fn, PC_B64), wide);
    reg_name(PC_B64, new_reg(fn, PC_B64), out->reg);
    sb_printf(&fn->body, "\tcvt.u64.u32 %s, %s;\n", wide, offset);
    sb_printf(&fn->body, "\tadd.u64 %s, %s, %s;\n", out->reg, base, wide);
  }
}

// The lane pointer plus index * stride + bytes, as a PTX address operand.
static void ptx_lane_address(PtxFn *fn, const PtxLanePointer *pointer,
                             unsigned index, unsigned bytes,
                             char address[48]) {
  if (pointer->stride_known) {
    unsigned offset = index * pointer->stride_bytes + bytes;
    if (offset)
      snprintf(address, 48, "%s+%u", pointer->reg, offset);
    else
      snprintf(address, 48, "%s", pointer->reg);
    return;
  }
  char offset[24];
  reg_name(PC_B32, new_reg(fn, PC_B32), offset);
  sb_printf(&fn->body, "\tmad.lo.u32 %s, %s, %u, %u;\n", offset,
            pointer->stride_reg, index, bytes);
  if (pointer->narrow) {
    char sum[24];
    reg_name(PC_B32, new_reg(fn, PC_B32), sum);
    sb_printf(&fn->body, "\tadd.u32 %s, %s, %s;\n", sum, pointer->reg,
              offset);
    snprintf(address, 48, "%s", sum);
  } else {
    char wide[24], sum[24];
    reg_name(PC_B64, new_reg(fn, PC_B64), wide);
    reg_name(PC_B64, new_reg(fn, PC_B64), sum);
    sb_printf(&fn->body, "\tcvt.u64.u32 %s, %s;\n", wide, offset);
    sb_printf(&fn->body, "\tadd.u64 %s, %s, %s;\n", sum, pointer->reg, wide);
    snprintf(address, 48, "%s", sum);
  }
}

static void ptx_lane_load_scale(PtxFn *fn, const PtxLanePointer *pointer,
                                const char *space, MtlcTensorElement element,
                                unsigned index, int block, int destination) {
  char address[48], value[24];
  unsigned element_bytes = element == MTLC_TENSOR_ELEMENT_FLOAT16 ? 2u : 4u;
  ptx_lane_address(fn, pointer, index, (unsigned)block * element_bytes,
                   address);
  reg_name(PC_F32, destination, value);
  if (element == MTLC_TENSOR_ELEMENT_FLOAT16) {
    char half[24];
    reg_name(PC_B16, new_reg(fn, PC_B16), half);
    sb_printf(&fn->body, "\tld%s.b16 %s, [%s];\n", space, half, address);
    sb_printf(&fn->body, "\tcvt.f32.f16 %s, %s;\n", value, half);
  } else {
    sb_printf(&fn->body, "\tld%s.f32 %s, [%s];\n", space, value, address);
  }
}

// Block-scaled i8 tile, whole: K is any multiple of 32, one scale block per
// 32. For each K32 block, each m16 A fragment and n8 B fragment is loaded
// once and serves every subtile it meets, and each row's and column's scale
// is loaded once. Per subtile, mma.sync forms the exact int32 dot of the
// block, and the result is added to its f32 accumulator times
// scale_A[row] * scale_B[column]. Row-major A and column-major B
// (untransposed) load four consecutive K bytes per 32-bit read -- a runtime
// precondition, like WMMA's: base addresses and leading dimensions of 8-bit
// operands are multiples of 4. Other layouts load bytes.
//
// Workgroup operands with static leading dimensions that keep every row
// 16-byte aligned load through ldmatrix instead: one x4 per m16 A fragment
// (rows 0-7 / 8-15 by K bytes 0-15 / 16-31 are exactly a0..a3), and for a
// halves-packed B one x4 per n8 column group and four K blocks (the lane's
// word of each 16-byte group is its nibble word for that block) -- or, with
// exactly four column groups, one x4 per K block across them, loaded where
// that block starts, so only one block's words are ever live.
//
// The block dot starts from the bits of a float whose unit in the last
// place is 1 (1.5 * 2^23), so its bits read as that float plus the dot, and
// one exact subtraction replaces the quarter-rate cvt.rn.f32.s32. A
// halves-packed 4-bit B with zero point 8 (or signed nibbles) instead keeps
// each nibble in the high half of its byte: the byte is 16 * (q - 8)
// exactly, three bit operations per 32-bit word instead of seven, and the
// dot is 16 times the block dot -- which a start of 1.5 * 2^19, where the
// float step is 1/16, reads back as the block dot itself.
//
// When B's scales are f16 -- stored so, or f32 declared (b_scale_values)
// to hold f16 or bf16 values -- -start * scale_B is exact in f32 (two
// significant bits times eleven), so fma(dot bits, scale_B, -start *
// scale_B) is the block dot times scale_B with one rounding, and a second
// fma adds that times scale_A: two operations per element and block. f16 A
// scales take the mirror form; otherwise the subtraction, the scale
// product and an fma. Widened scales skip the per-block conversion.
//
// A swizzled operand (a tensor-map load's layout) loads through ldmatrix at
// lane address p ^ key, key = ((p >> 7) & (S/16 - 1)) << 4. Its rows are S
// bytes and K stays inside one, so bits 7 and up of a lane's address do not
// move with K, and 16 rows (A) or 8 columns (B) on are 2^9 bytes or more
// on: the key is one per lane, and each K step costs an add and an xor.
static void ptx_mma_swizzle_key(PtxFn *fn, const char *address,
                                unsigned swizzle, char key[24]) {
  unsigned bits = swizzle == 128u ? 3u : (swizzle == 64u ? 2u : 1u);
  reg_name(PC_B32, new_reg(fn, PC_B32), key);
  sb_printf(&fn->body, "\tbfe.u32 %s, %s, 7, %u;\n", key, address, bits);
  sb_printf(&fn->body, "\tshl.b32 %s, %s, 4;\n", key, key);
}

static void ptx_mma_swizzled_address(PtxFn *fn, const char *lane,
                                     const char *key, unsigned bytes,
                                     char out[24]) {
  reg_name(PC_B32, new_reg(fn, PC_B32), out);
  if (bytes)
    sb_printf(&fn->body, "\tadd.u32 %s, %s, %u;\n", out, lane, bytes);
  sb_printf(&fn->body, "\txor.b32 %s, %s, %s;\n", out, bytes ? out : lane,
            key);
}

static int ptx_mma_reserve_registers(PtxFn *fn, PtxClass cls, int count) {
  int base = fn->count[cls];
  for (int i = 0; i < count; i++) new_reg(fn, cls);
  return base;
}

static void ptx_mma_load_f32_accumulator_tile(
    PtxFn *fn, const PtxMmaTileMemory *memory, MtlcTensorLayout layout,
    int m_tiles, int n_tiles, const char *group, const char *thread,
    int accumulator_base) {
  for (int mt = 0; mt < m_tiles; mt++) {
    for (int nt = 0; nt < n_tiles; nt++) {
      int acc = accumulator_base + (mt * n_tiles + nt) * 4;
      for (int e = 0; e < 4; e++) {
        PtxMmaCoordinate row = {1, 0,
                                (unsigned)mt * 16u + (e >= 2 ? 8u : 0u)};
        PtxMmaCoordinate column = {0, 2,
                                   (unsigned)nt * 8u + (unsigned)(e & 1)};
        ptx_mma_load_c(fn, memory, layout, row, column, group, thread,
                       acc + e);
      }
    }
  }
}

static void ptx_mma_store_f32_accumulator_tile(
    PtxFn *fn, const IRInstruction *in, const PtxMmaProfile *profile,
    const PtxMmaTileMemory *memory, const char *group, const char *thread,
    int accumulator_base) {
  int m_tiles = profile->m_tiles, n_tiles = profile->n_tiles;
  for (int mt = 0; mt < m_tiles; mt++) {
    for (int nt = 0; nt < n_tiles; nt++) {
      ptx_mma_store_f32_accumulator_subtile(
          fn, in, profile, memory->bases[3], memory->spaces[3],
          memory->strides[3], group, thread, (unsigned)mt * 16u,
          (unsigned)nt * 8u, accumulator_base + (mt * n_tiles + nt) * 4);
    }
  }
}

// A ring of four dot tuples: each block dot's epilogue runs three MMAs
// later, so the tensor pipe has the next products queued meanwhile (two
// measured ~0.4% slower on the int8 TMA GEMM, four ~0.5%).
enum { DOT_RING = 4, DOT_LAG = 3 };

typedef struct {
  PtxFn *fn;
  const MtlcTensorMmaDesc *desc;
  const PtxMmaProfile *profile;
  const PtxMmaTileMemory *memory;
  const char *group;
  const char *thread;
  int m_tiles;
  int n_tiles;
  int k_blocks;
  int fast_a;
  int fast_b;
  int a_ldsm;
  int b_ldsm;
  int x16;
  int prefactor_a;
  int prefactor_b;
  const char *start_float;
  const char *neg_start;
  char lane_id[24];
  PtxLanePointer a_lane;
  PtxLanePointer b_lane;
  PtxLanePointer nibble_lane;
  PtxLanePointer nib32_lane;
  PtxLanePointer sa_lane;
  PtxLanePointer sb_lane;
  char a_key[24];
  char b_key[24];
  char nib32_key[24];
  int raw_per_kb;
  int raw_base;
  char bias[24];
  int accumulator_base;
  int dot_base;
  int a_base;
  int b_base;
  int shift_base;
  int sa_base;
  int sb_base;
  int pre_base;
  int f_base;
} PtxS8ScaledTile;

static void ptx_s8_scaled_a_lane(PtxS8ScaledTile *tile) {
  PtxFn *fn = tile->fn;
  const MtlcTensorMmaDesc *desc = tile->desc;
  const PtxMmaTileMemory *memory = tile->memory;
  const char *lane_id = tile->lane_id;
  // A: per-lane pointers. The ldmatrix lane addresses row (l / 8 & 1) * 8
  // + l % 8, K bytes (l / 16) * 16; the plain path reads rows group (+8),
  // K bytes 4 * thread (+16).
  char lda_text[24];
  const char *lda = ptx_static_ld(desc->a_leading_dimension,
                                  memory->strides[0], lda_text);
  if (tile->a_ldsm) {
    char row[24], hi[24], r8[24];
    reg_name(PC_B32, new_reg(fn, PC_B32), row);
    reg_name(PC_B32, new_reg(fn, PC_B32), hi);
    reg_name(PC_B32, new_reg(fn, PC_B32), r8);
    sb_printf(&fn->body, "\tand.b32 %s, %s, 7;\n", row, lane_id);
    sb_printf(&fn->body, "\tbfe.u32 %s, %s, 3, 1;\n", r8, lane_id);
    sb_printf(&fn->body, "\tmad.lo.u32 %s, %s, 8, %s;\n", row, r8, row);
    sb_printf(&fn->body, "\tshr.u32 %s, %s, 4;\n", hi, lane_id);
    ptx_lane_pointer(fn, memory->bases[0], memory->spaces[0], lda, 1, row, 1,
                     hi, 16, &tile->a_lane);
  } else if (tile->fast_a) {
    ptx_lane_pointer(fn, memory->bases[0], memory->spaces[0], lda, 1,
                     tile->group, 1, tile->thread, 4, &tile->a_lane);
  }
  if (desc->a_swizzle)
    ptx_mma_swizzle_key(fn, tile->a_lane.reg, desc->a_swizzle, tile->a_key);
}

static void ptx_s8_scaled_nibble_ld(PtxS8ScaledTile *tile, char out[24]) {
  unsigned ldb_static = tile->desc->b_leading_dimension;
  if (ldb_static) {
    snprintf(out, 24, "%u", ldb_static / 2u);
  } else {
    reg_name(PC_B32, new_reg(tile->fn, PC_B32), out);
    sb_printf(&tile->fn->body, "\tshr.u32 %s, %s, 1;\n", out,
              tile->memory->strides[1]);
  }
}

static void ptx_s8_scaled_b_lanes(PtxS8ScaledTile *tile) {
  PtxFn *fn = tile->fn;
  const MtlcTensorMmaDesc *desc = tile->desc;
  const PtxMmaTileMemory *memory = tile->memory;
  const char *lane_id = tile->lane_id;
  // B: per-lane pointers. Halves-packed column n is a run of 16-byte
  // groups at n * ldb / 2 bytes; the ldmatrix lane addresses column l % 8,
  // group l / 8 (K blocks 4j .. 4j + 3 per x4).
  char ldb_text[24];
  const char *ldb = ptx_static_ld(desc->b_leading_dimension,
                                  memory->strides[1], ldb_text);
  if (tile->profile->b_halves) {
    char ld_bytes[24];
    ptx_s8_scaled_nibble_ld(tile, ld_bytes);
    if (tile->b_ldsm) {
      char col[24], grp[24];
      reg_name(PC_B32, new_reg(fn, PC_B32), col);
      reg_name(PC_B32, new_reg(fn, PC_B32), grp);
      sb_printf(&fn->body, "\tand.b32 %s, %s, 7;\n", col, lane_id);
      sb_printf(&fn->body, "\tshr.u32 %s, %s, 3;\n", grp, lane_id);
      ptx_lane_pointer(fn, memory->bases[1], memory->spaces[1], ld_bytes, 1,
                       col, 1, grp, 16, &tile->nibble_lane);
      if (desc->b_swizzle)
        ptx_mma_swizzle_key(fn, tile->nibble_lane.reg, desc->b_swizzle,
                            tile->b_key);
    } else {
      ptx_lane_pointer(fn, memory->bases[1], memory->spaces[1], ld_bytes, 1,
                       tile->group, 1, tile->thread, 4, &tile->nibble_lane);
    }
  } else if (tile->fast_b) {
    ptx_lane_pointer(fn, memory->bases[1], memory->spaces[1], ldb, 1,
                     tile->group, 1, tile->thread, 4, &tile->b_lane);
  }
}

static void ptx_s8_scaled_preload_nibbles(PtxS8ScaledTile *tile) {
  PtxFn *fn = tile->fn;
  const PtxLanePointer *nibble_lane = &tile->nibble_lane;
  int n_tiles = tile->n_tiles, k_blocks = tile->k_blocks;
  int raw_base = tile->raw_base;
  char b_swizzled[8][24];  // per K step, shared by every column group
  for (int nt = 0; nt < n_tiles; nt++) {
    int kb = 0;
    while (kb < k_blocks) {
      char address[48];
      if (tile->b_ldsm) {
        int count = k_blocks - kb >= 4 ? 4 : (k_blocks - kb >= 2 ? 2 : 1);
        char regs[4][24];
        for (int i = 0; i < count; i++)
          reg_name(PC_B32, raw_base + nt * k_blocks + kb + i, regs[i]);
        if (tile->desc->b_swizzle) {
          if (nt == 0)
            ptx_mma_swizzled_address(fn, nibble_lane->reg, tile->b_key,
                                     (unsigned)kb * 16u, b_swizzled[kb]);
          snprintf(address, sizeof(address), "%s+%u", b_swizzled[kb],
                   (unsigned)nt * 8u * nibble_lane->stride_bytes);
        } else {
          ptx_lane_address(fn, nibble_lane, (unsigned)nt * 8u,
                           (unsigned)kb * 16u, address);
        }
        if (count == 4)
          sb_printf(&fn->body,
                    "\tldmatrix.sync.aligned.m8n8.x4.shared.b16 {%s, %s, %s, %s}, [%s];\n",
                    regs[0], regs[1], regs[2], regs[3], address);
        else if (count == 2)
          sb_printf(&fn->body,
                    "\tldmatrix.sync.aligned.m8n8.x2.shared.b16 {%s, %s}, [%s];\n",
                    regs[0], regs[1], address);
        else
          sb_printf(&fn->body,
                    "\tldmatrix.sync.aligned.m8n8.x1.shared.b16 {%s}, [%s];\n",
                    regs[0], address);
        kb += count;
      } else {
        char word[24];
        reg_name(PC_B32, raw_base + nt * k_blocks + kb, word);
        ptx_lane_address(fn, nibble_lane, (unsigned)nt * 8u,
                         (unsigned)kb * 16u, address);
        sb_printf(&fn->body, "\tld%s.u32 %s, [%s];\n",
                  tile->memory->spaces[1], word, address);
        kb++;
      }
    }
  }
}

static void ptx_s8_scaled_raw_nibbles(PtxS8ScaledTile *tile) {
  PtxFn *fn = tile->fn;
  const MtlcTensorMmaDesc *desc = tile->desc;
  const PtxMmaTileMemory *memory = tile->memory;
  // Raw nibble words, [n_tile][block] -- or, with four column tiles on
  // ldmatrix, one K block's words at a time ([n_tile], reloaded every
  // block): an x4 load whose four matrices are the four tiles, the lane
  // addressing column l of the 32. Holding every block's words from the
  // start cost 3 * n_tiles registers more for the whole tile, and the int8
  // TMA GEMM ~2.5% (91.3 -> 93.6 TOPS at its widest shape).
  tile->raw_base = -1;
  tile->raw_per_kb = tile->profile->b_halves && tile->b_ldsm &&
                     tile->n_tiles == 4 && tile->k_blocks > 1;
  if (tile->raw_per_kb) {
    char ld_bytes32[24];
    ptx_s8_scaled_nibble_ld(tile, ld_bytes32);
    ptx_lane_pointer(fn, memory->bases[1], memory->spaces[1], ld_bytes32, 1,
                     tile->lane_id, 1, NULL, 0, &tile->nib32_lane);
    if (desc->b_swizzle)
      ptx_mma_swizzle_key(fn, tile->nib32_lane.reg, desc->b_swizzle,
                          tile->nib32_key);
    tile->raw_base = ptx_mma_reserve_registers(fn, PC_B32, tile->n_tiles);
  } else if (tile->profile->b_halves) {
    tile->raw_base = ptx_mma_reserve_registers(
        fn, PC_B32, tile->n_tiles * tile->k_blocks);
    ptx_s8_scaled_preload_nibbles(tile);
  }
}

static void ptx_s8_scaled_scale_lanes(PtxS8ScaledTile *tile) {
  const MtlcTensorMmaDesc *desc = tile->desc;
  const PtxMmaTileMemory *memory = tile->memory;
  int k_blocks = tile->k_blocks;
  // Scales: A is row-major [M, blocks], B column-major [blocks, N]. The
  // lane's rows are group + const, its columns 2 * thread + const.
  unsigned sa_bytes =
      desc->a_scale_element == MTLC_TENSOR_ELEMENT_FLOAT16 ? 2u : 4u;
  unsigned sb_bytes =
      desc->b_scale_element == MTLC_TENSOR_ELEMENT_FLOAT16 ? 2u : 4u;
  char lsa_text[24], lsb_text[24];
  const char *lsa = ptx_static_ld(
      desc->a_scale_leading_dimension ? desc->a_scale_leading_dimension
                                      : (uint32_t)k_blocks,
      memory->scale_strides[0], lsa_text);
  const char *lsb = ptx_static_ld(
      desc->b_scale_leading_dimension ? desc->b_scale_leading_dimension
                                      : (uint32_t)k_blocks,
      memory->scale_strides[1], lsb_text);
  ptx_lane_pointer(tile->fn, memory->bases[4], memory->spaces[4], lsa,
                   sa_bytes, tile->group, 1, NULL, 0, &tile->sa_lane);
  ptx_lane_pointer(tile->fn, memory->bases[5], memory->spaces[5], lsb,
                   sb_bytes, tile->thread, 2, NULL, 0, &tile->sb_lane);
}

static void ptx_s8_scaled_load_a(PtxS8ScaledTile *tile, unsigned k0) {
  PtxFn *fn = tile->fn;
  const MtlcTensorMmaDesc *desc = tile->desc;
  const PtxMmaTileMemory *memory = tile->memory;
  const PtxLanePointer *a_lane = &tile->a_lane;
  int a_base = tile->a_base;
  char a_swizzled[24];
  if (desc->a_swizzle)
    ptx_mma_swizzled_address(fn, a_lane->reg, tile->a_key, k0, a_swizzled);
  // A fragments for this block: register r holds rows group + 8*(r&1),
  // K k0 + 16*(r>>1) + 4*thread .. +3.
  for (int mt = 0; mt < tile->m_tiles; mt++) {
    if (tile->a_ldsm) {
      char address[48], r0[24], r1[24], r2[24], r3[24];
      reg_name(PC_B32, a_base + mt * 4, r0);
      reg_name(PC_B32, a_base + mt * 4 + 1, r1);
      reg_name(PC_B32, a_base + mt * 4 + 2, r2);
      reg_name(PC_B32, a_base + mt * 4 + 3, r3);
      if (desc->a_swizzle)
        snprintf(address, sizeof(address), "%s+%u", a_swizzled,
                 (unsigned)mt * 16u * a_lane->stride_bytes);
      else
        ptx_lane_address(fn, a_lane, (unsigned)mt * 16u, k0, address);
      sb_printf(&fn->body,
                "\tldmatrix.sync.aligned.m8n8.x4.shared.b16 {%s, %s, %s, %s}, [%s];\n",
                r0, r1, r2, r3, address);
      continue;
    }
    for (int r = 0; r < 4; r++) {
      unsigned row_off = (unsigned)mt * 16u + ((r & 1) ? 8u : 0u);
      unsigned k_off = k0 + ((r >> 1) ? 16u : 0u);
      if (tile->fast_a) {
        char address[48], dst[24];
        ptx_lane_address(fn, a_lane, row_off, k_off, address);
        reg_name(PC_B32, a_base + mt * 4 + r, dst);
        sb_printf(&fn->body, "\tld%s.u32 %s, [%s];\n", memory->spaces[0],
                  dst, address);
      } else {
        PtxMmaCoordinate rows[4], columns[4];
        for (int byte = 0; byte < 4; byte++) {
          rows[byte] = (PtxMmaCoordinate){1, 0, row_off};
          columns[byte] = (PtxMmaCoordinate){0, 4, k_off + (unsigned)byte};
        }
        ptx_mma_load_narrow_containers(
            fn, memory->bases[0], memory->strides[0], desc->a_layout,
            desc->transpose_a, desc->a_packing, memory->spaces[0], rows,
            columns, tile->group, tile->thread, 8, a_base + mt * 4 + r, 0);
      }
    }
  }
}

static void ptx_s8_scaled_load_block_nibbles(PtxS8ScaledTile *tile, int kb) {
  PtxFn *fn = tile->fn;
  char regs[4][24], address[48];
  for (int i = 0; i < 4; i++) reg_name(PC_B32, tile->raw_base + i, regs[i]);
  if (tile->desc->b_swizzle) {
    char sw[24];
    ptx_mma_swizzled_address(fn, tile->nib32_lane.reg, tile->nib32_key,
                             (unsigned)kb * 16u, sw);
    snprintf(address, sizeof(address), "%s", sw);
  } else {
    ptx_lane_address(fn, &tile->nib32_lane, 0, (unsigned)kb * 16u, address);
  }
  sb_printf(&fn->body,
            "\tldmatrix.sync.aligned.m8n8.x4.shared.b16 {%s, %s, %s, %s}, [%s];\n",
            regs[0], regs[1], regs[2], regs[3], address);
}

static void ptx_s8_scaled_unpack_nibbles(PtxS8ScaledTile *tile, int kb,
                                         int nt, const char *lo,
                                         const char *hi) {
  PtxFn *fn = tile->fn;
  const PtxMmaProfile *profile = tile->profile;
  char word[24];
  reg_name(PC_B32,
           tile->raw_per_kb ? tile->raw_base + nt
                            : tile->raw_base + nt * tile->k_blocks + kb,
           word);
  if (tile->x16) {
    // High nibbles stay put, low ones move up a nibble; ^ 8 per
    // nibble subtracts the zero point (signed nibbles need none).
    const char *flip = profile->b_signed4 ? "0x00000000" : "0x88888888";
    const char *flip_lo = profile->b_signed4 ? "0x00000000" : "0x80808080";
    char shifted[24];
    reg_name(PC_B32, tile->shift_base + nt, shifted);
    sb_printf(&fn->body, "\tshl.b32 %s, %s, 4;\n", shifted, word);
    sb_printf(&fn->body, "\tlop3.b32 %s, %s, %s, 0xF0F0F0F0, 0x28;\n", lo,
              shifted, flip_lo);
    sb_printf(&fn->body, "\tlop3.b32 %s, %s, %s, 0xF0F0F0F0, 0x28;\n", hi,
              word, flip);
  } else {
    sb_printf(&fn->body, "\tand.b32 %s, %s, 0x0F0F0F0F;\n", lo, word);
    sb_printf(&fn->body, "\tshr.u32 %s, %s, 4;\n", hi, word);
    sb_printf(&fn->body, "\tand.b32 %s, %s, 0x0F0F0F0F;\n", hi, hi);
    unsigned zero_point = profile->b_signed4 ? 8u : profile->b_zero_point;
    const char *halves[2] = {lo, hi};
    for (int h = 0; h < 2 && zero_point; h++) {
      if (profile->b_signed4)
        sb_printf(&fn->body, "\txor.b32 %s, %s, 0x08080808;\n", halves[h],
                  halves[h]);
      sb_printf(&fn->body, "\tadd.u32 %s, %s, 0x%08X;\n", halves[h],
                halves[h], (0x80u - zero_point) * 0x01010101u);
      sb_printf(&fn->body, "\txor.b32 %s, %s, 0x80808080;\n", halves[h],
                halves[h]);
    }
  }
}

static void ptx_s8_scaled_load_b(PtxS8ScaledTile *tile, int kb, unsigned k0) {
  PtxFn *fn = tile->fn;
  const MtlcTensorMmaDesc *desc = tile->desc;
  const PtxMmaTileMemory *memory = tile->memory;
  int b_base = tile->b_base;
  for (int nt = 0; nt < tile->n_tiles; nt++) {
    char lo[24], hi[24];
    reg_name(PC_B32, b_base + nt * 2, lo);
    reg_name(PC_B32, b_base + nt * 2 + 1, hi);
    if (tile->profile->b_halves) {
      ptx_s8_scaled_unpack_nibbles(tile, kb, nt, lo, hi);
      continue;
    }
    for (int r = 0; r < 2; r++) {
      unsigned col_off = (unsigned)nt * 8u;
      unsigned k_off = k0 + (r ? 16u : 0u);
      int dst_reg = b_base + nt * 2 + r;
      if (tile->fast_b) {
        char address[48], dst[24];
        ptx_lane_address(fn, &tile->b_lane, col_off, k_off, address);
        reg_name(PC_B32, dst_reg, dst);
        sb_printf(&fn->body, "\tld%s.u32 %s, [%s];\n", memory->spaces[1],
                  dst, address);
      } else {
        PtxMmaCoordinate rows[4], columns[4];
        for (int byte = 0; byte < 4; byte++) {
          rows[byte] = (PtxMmaCoordinate){0, 4, k_off + (unsigned)byte};
          columns[byte] = (PtxMmaCoordinate){1, 0, col_off};
        }
        ptx_mma_load_narrow_containers(
            fn, memory->bases[1], memory->strides[1], desc->b_layout,
            desc->transpose_b, desc->b_packing, memory->spaces[1], rows,
            columns, tile->group, tile->thread, 8, dst_reg, 0);
      }
    }
  }
}

static void ptx_s8_scaled_block_mma(PtxS8ScaledTile *tile, int mt, int nt,
                                    int g) {
  const char *bias = tile->bias;
  char a[128], b[128], dots[128];
  ptx_reg_tuple_at(PC_B32, tile->a_base + mt * 4, 4, a, sizeof(a));
  ptx_reg_tuple_at(PC_B32, tile->b_base + nt * 2, 2, b, sizeof(b));
  ptx_reg_tuple_at(PC_B32, tile->dot_base + (g % DOT_RING) * 4, 4, dots,
                   sizeof(dots));
  sb_printf(&tile->fn->body,
            "\tmma.sync.aligned.m16n8k32.row.col.s32.%s.%s.s32 %s, %s, %s, {%s, %s, %s, %s};\n",
            tile->profile->a_type, tile->profile->b_type, dots, a, b, bias,
            bias, bias, bias);
}

static void ptx_s8_scaled_load_scales(PtxS8ScaledTile *tile, int dkb) {
  PtxFn *fn = tile->fn;
  const MtlcTensorMmaDesc *desc = tile->desc;
  const PtxMmaTileMemory *memory = tile->memory;
  // Block dkb's scales, and the exact prefactors -start * scale.
  for (int mt = 0; mt < tile->m_tiles; mt++) {
    for (int h = 0; h < 2; h++) {
      ptx_lane_load_scale(fn, &tile->sa_lane, memory->spaces[4],
                          desc->a_scale_element,
                          (unsigned)mt * 16u + (h ? 8u : 0u), dkb,
                          tile->sa_base + mt * 2 + h);
      if (tile->prefactor_a) {
        char pre[24], sa[24];
        reg_name(PC_F32, tile->pre_base + mt * 2 + h, pre);
        reg_name(PC_F32, tile->sa_base + mt * 2 + h, sa);
        sb_printf(&fn->body, "\tmul.rn.f32 %s, %s, %s;\n", pre, sa,
                  tile->neg_start);
      }
    }
  }
  for (int nt = 0; nt < tile->n_tiles; nt++) {
    for (int h = 0; h < 2; h++) {
      ptx_lane_load_scale(fn, &tile->sb_lane, memory->spaces[5],
                          desc->b_scale_element,
                          (unsigned)nt * 8u + (unsigned)h, dkb,
                          tile->sb_base + nt * 2 + h);
      if (tile->prefactor_b) {
        char pre[24], sb[24];
        reg_name(PC_F32, tile->pre_base + nt * 2 + h, pre);
        reg_name(PC_F32, tile->sb_base + nt * 2 + h, sb);
        sb_printf(&fn->body, "\tmul.rn.f32 %s, %s, %s;\n", pre, sb,
                  tile->neg_start);
      }
    }
  }
}

static void ptx_s8_scaled_epilogue(PtxS8ScaledTile *tile, int mt, int nt,
                                   int done) {
  PtxFn *fn = tile->fn;
  int prefactor_a = tile->prefactor_a, prefactor_b = tile->prefactor_b;
  int pre_base = tile->pre_base;
  int acc = tile->accumulator_base + (mt * tile->n_tiles + nt) * 4;
  for (int e = 0; e < 4; e++) {
    char dot[24], f[24], sa[24], sb[24], accr[24];
    reg_name(PC_B32, tile->dot_base + (done % DOT_RING) * 4 + e, dot);
    reg_name(PC_F32, tile->f_base + e, f);
    reg_name(PC_F32, tile->sa_base + mt * 2 + (e >= 2 ? 1 : 0), sa);
    reg_name(PC_F32, tile->sb_base + nt * 2 + (e & 1), sb);
    reg_name(PC_F32, acc + e, accr);
    sb_printf(&fn->body, "\tmov.b32 %s, %s;\n", f, dot);
    if (prefactor_b || prefactor_a) {
      char pre[24];
      const char *first = prefactor_b ? sb : sa;
      const char *second = prefactor_b ? sa : sb;
      reg_name(PC_F32,
               pre_base + (prefactor_b ? nt * 2 + (e & 1)
                                       : mt * 2 + (e >= 2 ? 1 : 0)),
               pre);
      sb_printf(&fn->body, "\tfma.rn.f32 %s, %s, %s, %s;\n", f, f, first,
                pre);
      sb_printf(&fn->body, "\tfma.rn.f32 %s, %s, %s, %s;\n", accr, f,
                second, accr);
    } else {
      char scale[24];
      reg_name(PC_F32, new_reg(fn, PC_F32), scale);
      sb_printf(&fn->body, "\tsub.rn.f32 %s, %s, %s;\n", f, f,
                tile->start_float);
      sb_printf(&fn->body, "\tmul.rn.f32 %s, %s, %s;\n", scale, sa, sb);
      sb_printf(&fn->body, "\tfma.rn.f32 %s, %s, %s, %s;\n", accr, f,
                scale, accr);
    }
  }
}

static void ptx_s8_scaled_block_steps(PtxS8ScaledTile *tile, int kb) {
  int n_tiles = tile->n_tiles;
  // This block's dots, DOT_LAG MMAs ahead of their epilogues. The ring
  // runs on across blocks: a block's scales are loaded where its first
  // epilogue is due, after the previous block's last ones, so those sit
  // among this block's first MMAs instead of draining the tensor pipe at
  // every block boundary (~0.4% on the int8 TMA GEMM).
  int pairs = tile->m_tiles * n_tiles;
  int g0 = kb * pairs;
  int steps = kb == tile->k_blocks - 1 ? pairs + DOT_LAG : pairs;
  for (int step = 0; step < steps; step++) {
    int g = g0 + step;
    if (step < pairs)
      ptx_s8_scaled_block_mma(tile, step / n_tiles, step % n_tiles, g);
    int done = g - DOT_LAG;
    if (done < 0) continue;
    int dkb = done / pairs;
    int dstep = done % pairs;
    if (dstep == 0) ptx_s8_scaled_load_scales(tile, dkb);
    ptx_s8_scaled_epilogue(tile, dstep / n_tiles, dstep % n_tiles, done);
  }
}

static void ptx_s8_scaled_block(PtxS8ScaledTile *tile, int kb) {
  unsigned k0 = (unsigned)kb * 32u;
  ptx_s8_scaled_load_a(tile, k0);

  // B fragments: register r holds column group, K k0 + 16*r + 4*thread
  // .. +3.
  if (tile->raw_per_kb) ptx_s8_scaled_load_block_nibbles(tile, kb);
  ptx_s8_scaled_load_b(tile, kb, k0);

  ptx_s8_scaled_block_steps(tile, kb);
}

static void ptx_emit_mma_s8_scaled_tile(
    PtxFn *fn, const IRInstruction *in, const PtxMmaProfile *profile,
    const PtxMmaTileMemory *memory, const char *group, const char *thread,
    int accumulator_base, int load_accumulator, int store_accumulator) {
  const MtlcTensorMmaDesc *desc = &IR_TENSOR_MMA(in);
  PtxS8ScaledTile tile;
  memset(&tile, 0, sizeof(tile));
  tile.fn = fn;
  tile.desc = desc;
  tile.profile = profile;
  tile.memory = memory;
  tile.group = group;
  tile.thread = thread;
  int m_tiles = profile->m_tiles, n_tiles = profile->n_tiles;
  tile.m_tiles = m_tiles;
  tile.n_tiles = n_tiles;
  tile.k_blocks = desc->k / 32;
  if (accumulator_base < 0)
    accumulator_base =
        ptx_mma_reserve_registers(fn, PC_F32, m_tiles * n_tiles * 4);
  tile.accumulator_base = accumulator_base;
  tile.fast_a = desc->a_layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR &&
                !desc->transpose_a;
  tile.fast_b = desc->b_layout == MTLC_TENSOR_LAYOUT_COLUMN_MAJOR &&
                !desc->transpose_b;
  int ldsm_ok = fn->target_arch >= 75 && ptx_version_at_least(fn, 6, 5);
  unsigned lda_static = desc->a_leading_dimension;
  unsigned ldb_static = desc->b_leading_dimension;
  tile.a_ldsm = ldsm_ok && tile.fast_a && memory->spaces[0] &&
                strcmp(memory->spaces[0], ".shared") == 0 && lda_static &&
                lda_static % 16u == 0;
  tile.b_ldsm = ldsm_ok && profile->b_halves && memory->spaces[1] &&
                strcmp(memory->spaces[1], ".shared") == 0 && ldb_static &&
                (ldb_static / 2u) % 16u == 0;
  if ((desc->a_swizzle && !tile.a_ldsm) || (desc->b_swizzle && !tile.b_ldsm)) {
    fn_error(fn, "PTX swizzled MMA operands are read from workgroup memory "
                 "through ldmatrix (sm_75, PTX 6.5)");
    return;
  }
  tile.x16 = profile->b_halves &&
             (profile->b_signed4 || profile->b_zero_point == 8);
  // -start, for the exact prefactor: f32 bits and hex text.
  const char *start_bits = tile.x16 ? "0x49400000" : "0x4B400000";
  tile.start_float = tile.x16 ? "0f49400000" : "0f4B400000";
  tile.neg_start = tile.x16 ? "0fC9400000" : "0fCB400000";
  // Scales with at most 11 significant bits (f16, or f32 declared to hold
  // f16 or bf16 values) make -start * scale exact.
  tile.prefactor_b =
      ir_tensor_block_scale_order(desc) == IR_BLOCK_SCALE_B_FIRST;
  tile.prefactor_a =
      ir_tensor_block_scale_order(desc) == IR_BLOCK_SCALE_A_FIRST;

  reg_name(PC_B32, new_reg(fn, PC_B32), tile.lane_id);
  sb_printf(&fn->body, "\tmad.lo.u32 %s, %s, 4, %s;\n", tile.lane_id, group,
            thread);

  ptx_s8_scaled_a_lane(&tile);
  ptx_s8_scaled_b_lanes(&tile);
  ptx_s8_scaled_raw_nibbles(&tile);
  ptx_s8_scaled_scale_lanes(&tile);

  if (load_accumulator)
    ptx_mma_load_f32_accumulator_tile(fn, memory, desc->c_layout, m_tiles,
                                      n_tiles, group, thread,
                                      accumulator_base);

  reg_name(PC_B32, new_reg(fn, PC_B32), tile.bias);
  sb_printf(&fn->body, "\tmov.b32 %s, %s;\n", tile.bias, start_bits);
  tile.dot_base = ptx_mma_reserve_registers(fn, PC_B32, DOT_RING * 4);

  // Fragment, scale and prefactor registers are named once and reused by
  // every block: the reuse keeps ptxas from hoisting all the blocks' loads
  // ahead of their MMAs (measured: ~40 more live registers and a register
  // shuffle per MMA when each block had fresh names).
  tile.a_base = ptx_mma_reserve_registers(fn, PC_B32, m_tiles * 4);
  tile.b_base = ptx_mma_reserve_registers(fn, PC_B32, n_tiles * 2);
  tile.shift_base = ptx_mma_reserve_registers(fn, PC_B32, n_tiles);
  tile.sa_base = ptx_mma_reserve_registers(fn, PC_F32, m_tiles * 2);
  tile.sb_base = ptx_mma_reserve_registers(fn, PC_F32, n_tiles * 2);
  int pre_count = tile.prefactor_b ? n_tiles * 2
                                   : (tile.prefactor_a ? m_tiles * 2 : 0);
  tile.pre_base = ptx_mma_reserve_registers(fn, PC_F32, pre_count);
  tile.f_base = ptx_mma_reserve_registers(fn, PC_F32, 4);
  for (int kb = 0; kb < tile.k_blocks; kb++) ptx_s8_scaled_block(&tile, kb);

  if (store_accumulator)
    ptx_mma_store_f32_accumulator_tile(fn, in, profile, memory, group, thread,
                                       accumulator_base);
}

// f16/bf16 tile with a C row scale: mma.sync m16n8k16 with the documented
// fragment layouts, since scaling rows needs to know which rows a lane
// holds (WMMA accumulators are opaque). C/D element e of an m16n8 subtile
// sits at row group + 8 * (e >= 2), column 2 * thread + (e & 1): C (or the
// resident accumulator) is multiplied by c_scale[row] before the K steps.
// A register r holds row group + 8 * (r & 1), K 2 * thread + 8 * (r >> 1)
// and the next K; B register r holds column group, K 2 * thread + 8 * r and
// the next. Row-major A and column-major B load those pairs as single
// 32-bit reads (a runtime precondition, as for WMMA: 4-byte aligned bases
// and even leading dimensions); row-major B in workgroup memory with a
// static leading dimension that keeps rows 16-byte aligned comes in four
// 8x8 transposed blocks per ldmatrix, two n8 subtiles a K step. Anything
// else loads halves.
static void ptx_f16_load_pair(PtxFn *fn, const char *base,
                              const char *leading_dimension,
                              MtlcTensorLayout layout, int transpose,
                              const char *space, PtxMmaCoordinate row0,
                              PtxMmaCoordinate column0,
                              PtxMmaCoordinate row1,
                              PtxMmaCoordinate column1, const char *group,
                              const char *thread, int destination) {
  char address0[24], address1[24], lo[24], hi[24], dst[24];
  ptx_mma_emit_address(fn, base, leading_dimension, layout, transpose, row0,
                       column0, 2, group, thread, address0);
  ptx_mma_emit_address(fn, base, leading_dimension, layout, transpose, row1,
                       column1, 2, group, thread, address1);
  reg_name(PC_B16, new_reg(fn, PC_B16), lo);
  reg_name(PC_B16, new_reg(fn, PC_B16), hi);
  reg_name(PC_B32, destination, dst);
  sb_printf(&fn->body, "\tld%s.b16 %s, [%s];\n", space, lo, address0);
  sb_printf(&fn->body, "\tld%s.b16 %s, [%s];\n", space, hi, address1);
  sb_printf(&fn->body, "\tmov.b32 %s, {%s, %s};\n", dst, lo, hi);
}

static void ptx_f16_tile_scale_rows(PtxFn *fn, const PtxMmaTileMemory *memory,
                                    int m_tiles, int n_tiles,
                                    const char *group,
                                    int accumulator_base) {
  PtxLanePointer scale_lane;
  ptx_lane_pointer(fn, memory->c_scale_base, memory->c_scale_space, "1", 4,
                   group, 1, NULL, 0, &scale_lane);
  for (int mt = 0; mt < m_tiles; mt++) {
    for (int h = 0; h < 2; h++) {
      char address[48], scale[24];
      ptx_lane_address(fn, &scale_lane, (unsigned)mt * 16u + 8u * (unsigned)h,
                       0, address);
      reg_name(PC_F32, new_reg(fn, PC_F32), scale);
      sb_printf(&fn->body, "\tld%s.f32 %s, [%s];\n", memory->c_scale_space,
                scale, address);
      for (int nt = 0; nt < n_tiles; nt++) {
        for (int e = 0; e < 2; e++) {
          char acc[24];
          reg_name(PC_F32, accumulator_base + (mt * n_tiles + nt) * 4 +
                               h * 2 + e,
                   acc);
          sb_printf(&fn->body, "\tmul.rn.f32 %s, %s, %s;\n", acc, acc,
                    scale);
        }
      }
    }
  }
}

static void ptx_f16_tile_matrix_b_pointer(PtxFn *fn,
                                          const PtxMmaTileMemory *memory,
                                          const char *group,
                                          const char *thread,
                                          unsigned ldb_static,
                                          char matrix_ptr[24]) {
  // Lane l addresses row (l / 8 & 1) * 8 + l % 8 of the K step at column
  // (l / 16) * 8 of the subtile pair: blocks (K 0-7, n8 0), (K 8-15,
  // n8 0), (K 0-7, n8 1), (K 8-15, n8 1) -- b0/b1 of two subtiles.
  char lane_id[24], quad[24], rowsel[24], offset[24], narrow[24];
  reg_name(PC_B32, new_reg(fn, PC_B32), lane_id);
  reg_name(PC_B32, new_reg(fn, PC_B32), quad);
  reg_name(PC_B32, new_reg(fn, PC_B32), rowsel);
  reg_name(PC_B32, new_reg(fn, PC_B32), offset);
  reg_name(PC_B32, new_reg(fn, PC_B32), narrow);
  reg_name(PC_B32, new_reg(fn, PC_B32), matrix_ptr);
  sb_printf(&fn->body, "\tmad.lo.u32 %s, %s, 4, %s;\n", lane_id, group,
            thread);
  sb_printf(&fn->body, "\tand.b32 %s, %s, 15;\n", rowsel, lane_id);
  sb_printf(&fn->body, "\tshr.u32 %s, %s, 4;\n", quad, lane_id);
  sb_printf(&fn->body, "\tmul.lo.u32 %s, %s, %u;\n", offset, rowsel,
            ldb_static * 2u);
  sb_printf(&fn->body, "\tmad.lo.u32 %s, %s, 16, %s;\n", offset, quad,
            offset);
  sb_printf(&fn->body, "\tcvt.u32.u64 %s, %s;\n", narrow,
            memory->bases[1]);
  sb_printf(&fn->body, "\tadd.u32 %s, %s, %s;\n", matrix_ptr, narrow,
            offset);
}

static void ptx_f16_tile_ldsm_pointers(PtxFn *fn,
                                       const PtxMmaTileMemory *memory,
                                       const char *group, const char *thread,
                                       int a_ldsm, int b_ldsm,
                                       unsigned lda_static,
                                       unsigned ldb_static, char a_lm[24],
                                       char b_lm[24]) {
  char lid[24], lo[24], sel[24], hi[24], off[24], narrow[24];
  reg_name(PC_B32, new_reg(fn, PC_B32), lid);
  reg_name(PC_B32, new_reg(fn, PC_B32), lo);
  reg_name(PC_B32, new_reg(fn, PC_B32), sel);
  reg_name(PC_B32, new_reg(fn, PC_B32), hi);
  sb_printf(&fn->body, "\tmad.lo.u32 %s, %s, 4, %s;\n", lid, group, thread);
  sb_printf(&fn->body, "\tand.b32 %s, %s, 7;\n", lo, lid);
  sb_printf(&fn->body, "\tbfe.u32 %s, %s, 3, 1;\n", sel, lid);
  sb_printf(&fn->body, "\tshr.u32 %s, %s, 4;\n", hi, lid);
  if (a_ldsm) {
    reg_name(PC_B32, new_reg(fn, PC_B32), off);
    reg_name(PC_B32, new_reg(fn, PC_B32), narrow);
    reg_name(PC_B32, new_reg(fn, PC_B32), a_lm);
    sb_printf(&fn->body, "\tmad.lo.u32 %s, %s, 8, %s;\n", off, sel, lo);
    sb_printf(&fn->body, "\tmul.lo.u32 %s, %s, %u;\n", off, off,
              lda_static * 2u);
    sb_printf(&fn->body, "\tmad.lo.u32 %s, %s, 16, %s;\n", off, hi, off);
    sb_printf(&fn->body, "\tcvt.u32.u64 %s, %s;\n", narrow,
              memory->bases[0]);
    sb_printf(&fn->body, "\tadd.u32 %s, %s, %s;\n", a_lm, narrow, off);
  }
  if (b_ldsm) {
    reg_name(PC_B32, new_reg(fn, PC_B32), off);
    reg_name(PC_B32, new_reg(fn, PC_B32), narrow);
    reg_name(PC_B32, new_reg(fn, PC_B32), b_lm);
    sb_printf(&fn->body, "\tmad.lo.u32 %s, %s, 8, %s;\n", off, hi, lo);
    sb_printf(&fn->body, "\tmul.lo.u32 %s, %s, %u;\n", off, off,
              ldb_static * 2u);
    sb_printf(&fn->body, "\tmad.lo.u32 %s, %s, 16, %s;\n", off, sel, off);
    sb_printf(&fn->body, "\tcvt.u32.u64 %s, %s;\n", narrow,
              memory->bases[1]);
    sb_printf(&fn->body, "\tadd.u32 %s, %s, %s;\n", b_lm, narrow, off);
  }
}

static void ptx_f16_tile_ldmatrix_a(PtxFn *fn, const char *a_lm,
                                    unsigned lda_static, int m_tiles, int ks,
                                    int a_base) {
  for (int mt = 0; mt < m_tiles; mt++) {
    char r[4][24];
    for (int i = 0; i < 4; i++) reg_name(PC_B32, a_base + mt * 4 + i, r[i]);
    sb_printf(&fn->body,
              "\tldmatrix.sync.aligned.m8n8.x4.shared.b16 {%s, %s, %s, %s}, [%s+%u];\n",
              r[0], r[1], r[2], r[3], a_lm,
              (unsigned)mt * 16u * lda_static * 2u + (unsigned)ks * 32u);
  }
}

static void ptx_f16_tile_load_a_words(PtxFn *fn,
                                      const MtlcTensorMmaDesc *desc,
                                      const PtxMmaTileMemory *memory,
                                      const PtxLanePointer *a_lane,
                                      int fast_a, int m_tiles, int ks,
                                      int a_base, const char *group,
                                      const char *thread) {
  for (int mt = 0; mt < m_tiles; mt++) {
    for (int r = 0; r < 4; r++) {
      unsigned row_off = (unsigned)mt * 16u + ((r & 1) ? 8u : 0u);
      unsigned k_off = (unsigned)ks * 16u + ((r >> 1) ? 8u : 0u);
      if (fast_a) {
        char address[48], dst[24];
        ptx_lane_address(fn, a_lane, row_off, k_off * 2u, address);
        reg_name(PC_B32, a_base + mt * 4 + r, dst);
        sb_printf(&fn->body, "\tld%s.b32 %s, [%s];\n", memory->spaces[0],
                  dst, address);
      } else {
        PtxMmaCoordinate row = {1, 0, row_off};
        PtxMmaCoordinate c0 = {0, 2, k_off};
        PtxMmaCoordinate c1 = {0, 2, k_off + 1u};
        ptx_f16_load_pair(fn, memory->bases[0], memory->strides[0],
                          desc->a_layout, desc->transpose_a,
                          memory->spaces[0], row, c0, row, c1, group,
                          thread, a_base + mt * 4 + r);
      }
    }
  }
}

static void ptx_f16_tile_ldmatrix_b_trans(PtxFn *fn, const char *matrix_ptr,
                                          unsigned ldb_static, int n_tiles,
                                          int ks, int b_base) {
  for (int nt = 0; nt < n_tiles; nt += 2) {
    unsigned offset = ((unsigned)ks * 16u * ldb_static +
                       (unsigned)nt * 8u) * 2u;
    char b0[24], b1[24], b2[24], b3[24];
    reg_name(PC_B32, b_base + nt * 2, b0);
    reg_name(PC_B32, b_base + nt * 2 + 1, b1);
    if (nt + 1 < n_tiles) {
      reg_name(PC_B32, b_base + nt * 2 + 2, b2);
      reg_name(PC_B32, b_base + nt * 2 + 3, b3);
      sb_printf(&fn->body,
                "\tldmatrix.sync.aligned.m8n8.x4.trans.shared.b16 {%s, %s, %s, %s}, [%s+%u];\n",
                b0, b1, b2, b3, matrix_ptr, offset);
    } else {
      sb_printf(&fn->body,
                "\tldmatrix.sync.aligned.m8n8.x2.trans.shared.b16 {%s, %s}, [%s+%u];\n",
                b0, b1, matrix_ptr, offset);
    }
  }
}

static void ptx_f16_tile_ldmatrix_b(PtxFn *fn, const char *b_lm,
                                    unsigned ldb_static, int n_tiles, int ks,
                                    int b_base) {
  for (int nt = 0; nt < n_tiles; nt += 2) {
    unsigned offset = (unsigned)nt * 8u * ldb_static * 2u + (unsigned)ks * 32u;
    char b0[24], b1[24], b2[24], b3[24];
    reg_name(PC_B32, b_base + nt * 2, b0);
    reg_name(PC_B32, b_base + nt * 2 + 1, b1);
    if (nt + 1 < n_tiles) {
      reg_name(PC_B32, b_base + nt * 2 + 2, b2);
      reg_name(PC_B32, b_base + nt * 2 + 3, b3);
      sb_printf(&fn->body,
                "\tldmatrix.sync.aligned.m8n8.x4.shared.b16 {%s, %s, %s, %s}, [%s+%u];\n",
                b0, b1, b2, b3, b_lm, offset);
    } else {
      sb_printf(&fn->body,
                "\tldmatrix.sync.aligned.m8n8.x2.shared.b16 {%s, %s}, [%s+%u];\n",
                b0, b1, b_lm, offset);
    }
  }
}

static void ptx_f16_tile_load_b_words(PtxFn *fn,
                                      const MtlcTensorMmaDesc *desc,
                                      const PtxMmaTileMemory *memory,
                                      const PtxLanePointer *b_lane,
                                      int fast_b, int n_tiles, int ks,
                                      int b_base, const char *group,
                                      const char *thread) {
  for (int nt = 0; nt < n_tiles; nt++) {
    for (int r = 0; r < 2; r++) {
      unsigned col_off = (unsigned)nt * 8u;
      unsigned k_off = (unsigned)ks * 16u + (r ? 8u : 0u);
      if (fast_b) {
        char address[48], dst[24];
        ptx_lane_address(fn, b_lane, col_off, k_off * 2u, address);
        reg_name(PC_B32, b_base + nt * 2 + r, dst);
        sb_printf(&fn->body, "\tld%s.b32 %s, [%s];\n", memory->spaces[1],
                  dst, address);
      } else {
        PtxMmaCoordinate r0 = {0, 2, k_off};
        PtxMmaCoordinate r1 = {0, 2, k_off + 1u};
        PtxMmaCoordinate column = {1, 0, col_off};
        ptx_f16_load_pair(fn, memory->bases[1], memory->strides[1],
                          desc->b_layout, desc->transpose_b,
                          memory->spaces[1], r0, column, r1, column,
                          group, thread, b_base + nt * 2 + r);
      }
    }
  }
}

static void ptx_f16_tile_mma(PtxFn *fn, const char *element, int m_tiles,
                             int n_tiles, int a_base, int b_base,
                             int accumulator_base) {
  for (int mt = 0; mt < m_tiles; mt++) {
    char a[128];
    ptx_reg_tuple_at(PC_B32, a_base + mt * 4, 4, a, sizeof(a));
    for (int nt = 0; nt < n_tiles; nt++) {
      char b[128], acc[128];
      ptx_reg_tuple_at(PC_B32, b_base + nt * 2, 2, b, sizeof(b));
      ptx_reg_tuple_at(PC_F32, accumulator_base + (mt * n_tiles + nt) * 4,
                       4, acc, sizeof(acc));
      sb_printf(&fn->body,
                "\tmma.sync.aligned.m16n8k16.row.col.f32.%s.%s.f32 %s, %s, %s, %s;\n",
                element, element, acc, a, b, acc);
    }
  }
}

static void ptx_emit_mma_f16_tile(
    PtxFn *fn, const IRInstruction *in, const PtxMmaProfile *profile,
    const PtxMmaTileMemory *memory, const char *group, const char *thread,
    int accumulator_base, int load_accumulator, int store_accumulator) {
  const MtlcTensorMmaDesc *desc = &IR_TENSOR_MMA(in);
  int m_tiles = profile->m_tiles, n_tiles = profile->n_tiles;
  int k_steps = desc->k / 16;
  if (accumulator_base < 0)
    accumulator_base =
        ptx_mma_reserve_registers(fn, PC_F32, m_tiles * n_tiles * 4);

  if (load_accumulator)
    ptx_mma_load_f32_accumulator_tile(fn, memory, desc->c_layout, m_tiles,
                                      n_tiles, group, thread,
                                      accumulator_base);

  if (desc->c_scale_mode == MTLC_TENSOR_SCALE_PER_ROW)
    ptx_f16_tile_scale_rows(fn, memory, m_tiles, n_tiles, group,
                            accumulator_base);

  int fast_a = desc->a_layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR &&
               !desc->transpose_a;
  int fast_b = desc->b_layout == MTLC_TENSOR_LAYOUT_COLUMN_MAJOR &&
               !desc->transpose_b;
  unsigned ldb_static = desc->b_leading_dimension;
  int matrix_b = !fast_b && desc->b_layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR &&
                 !desc->transpose_b && memory->spaces[1] &&
                 strcmp(memory->spaces[1], ".shared") == 0 &&
                 ldb_static != 0 && (ldb_static * 2u) % 16u == 0;
  PtxLanePointer a_lane, b_lane;
  char lda_text[24], ldb_text[24];
  const char *lda = ptx_static_ld(desc->a_leading_dimension,
                                  memory->strides[0], lda_text);
  const char *ldb = ptx_static_ld(desc->b_leading_dimension,
                                  memory->strides[1], ldb_text);
  if (fast_a)
    ptx_lane_pointer(fn, memory->bases[0], memory->spaces[0], lda, 2, group,
                     1, thread, 4, &a_lane);
  if (fast_b)
    ptx_lane_pointer(fn, memory->bases[1], memory->spaces[1], ldb, 2, group,
                     1, thread, 4, &b_lane);
  char matrix_ptr[24];
  if (matrix_b)
    ptx_f16_tile_matrix_b_pointer(fn, memory, group, thread, ldb_static,
                                  matrix_ptr);
  // ldmatrix for a row-major A and a column-major B in workgroup memory
  // whose rows are whole 16-byte groups: one x4 brings a 16 x 16 A fragment
  // (rows (l / 8 & 1) * 8 + l % 8, K (l / 16) * 8) or the b0/b1 pairs of
  // two n8 columns (columns (l / 16) * 8 + l % 8, K (l / 8 & 1) * 8), in the
  // register order the word loads below give -- a quarter of the loads.
  unsigned lda_static = desc->a_leading_dimension;
  int ldsm_ok = fn->target_arch >= 75 && ptx_version_at_least(fn, 6, 5);
  int a_ldsm = ldsm_ok && fast_a && memory->spaces[0] &&
               strcmp(memory->spaces[0], ".shared") == 0 && lda_static != 0 &&
               (lda_static * 2u) % 16u == 0;
  int b_ldsm = ldsm_ok && fast_b && memory->spaces[1] &&
               strcmp(memory->spaces[1], ".shared") == 0 && ldb_static != 0 &&
               (ldb_static * 2u) % 16u == 0;
  char a_lm[24], b_lm[24];
  if (a_ldsm || b_ldsm)
    ptx_f16_tile_ldsm_pointers(fn, memory, group, thread, a_ldsm, b_ldsm,
                               lda_static, ldb_static, a_lm, b_lm);
  const char *element = profile->a_type;
  for (int ks = 0; ks < k_steps; ks++) {
    int a_base = ptx_mma_reserve_registers(fn, PC_B32, m_tiles * 4);
    int b_base = ptx_mma_reserve_registers(fn, PC_B32, n_tiles * 2);
    if (a_ldsm)
      ptx_f16_tile_ldmatrix_a(fn, a_lm, lda_static, m_tiles, ks, a_base);
    else
      ptx_f16_tile_load_a_words(fn, desc, memory, &a_lane, fast_a, m_tiles,
                                ks, a_base, group, thread);
    if (matrix_b)
      ptx_f16_tile_ldmatrix_b_trans(fn, matrix_ptr, ldb_static, n_tiles, ks,
                                    b_base);
    else if (b_ldsm)
      ptx_f16_tile_ldmatrix_b(fn, b_lm, ldb_static, n_tiles, ks, b_base);
    else
      ptx_f16_tile_load_b_words(fn, desc, memory, &b_lane, fast_b, n_tiles,
                                ks, b_base, group, thread);
    ptx_f16_tile_mma(fn, element, m_tiles, n_tiles, a_base, b_base,
                     accumulator_base);
  }

  if (store_accumulator)
    ptx_mma_store_f32_accumulator_tile(fn, in, profile, memory, group, thread,
                                       accumulator_base);
}

static void ptx_emit_mma_native_subtile(
    PtxFn *fn, const IRInstruction *in, const PtxMmaProfile *profile,
    const PtxMmaTileMemory *memory, const char *group, const char *thread,
    const PtxMmaSparseAFragment *sparse_a, unsigned m_offset,
    unsigned n_offset, int accumulator_base, int load_accumulator,
    int store_accumulator) {
  if (ptx_mma_profile_is_sparse(profile)) {
    ptx_emit_mma_sparse_f16_subtile(
        fn, in, profile, memory, group, thread, sparse_a, m_offset, n_offset,
        accumulator_base, load_accumulator, store_accumulator);
  } else if (profile->kind == PTX_MMA_MXFP4 ||
      profile->kind == PTX_MMA_NVFP4) {
    ptx_emit_mma_fp4_subtile(
        fn, in, profile, memory, group, thread, m_offset, n_offset,
        accumulator_base, load_accumulator, store_accumulator);
  } else {
    ptx_emit_mma_byte_subtile(
        fn, in, profile, memory, group, thread, m_offset, n_offset,
        accumulator_base, load_accumulator, store_accumulator);
  }
}

static void ptx_emit_tensor_mma_native_single(PtxFn *fn,
                                               const IRInstruction *in,
                                               const PtxMmaProfile *profile) {
  size_t per_tile = ir_tensor_mma_operand_count(&IR_TENSOR_MMA(in));
  PtxMmaTileMemory memory;
  if (!per_tile || in->argument_count != per_tile) {
    fn_error(fn, "PTX native MMA tensor operand count is inconsistent");
    return;
  }
  if (!ptx_mma_prepare_tile_memory(fn, in, profile, 0, per_tile, &memory)) {
    fn_error(fn,
             "PTX native MMA has invalid tile pointers or stride operands");
    return;
  }

  char lane[24], group[24], thread[24];
  reg_name(PC_B32, new_reg(fn, PC_B32), lane);
  reg_name(PC_B32, new_reg(fn, PC_B32), group);
  reg_name(PC_B32, new_reg(fn, PC_B32), thread);
  sb_printf(&fn->body,
            "\t// mtlc.tensor_mma native-mma %s whole-tile lowering\n",
            ptx_mma_kind_name(profile));
  sb_printf(&fn->body, "\tmov.u32 %s, %%laneid;\n", lane);
  sb_printf(&fn->body, "\tshr.u32 %s, %s, 2;\n", group, lane);
  sb_printf(&fn->body, "\tand.b32 %s, %s, 3;\n", thread, lane);
  if (profile->kind == PTX_MMA_S8_SCALED) {
    ptx_emit_mma_s8_scaled_tile(fn, in, profile, &memory, group, thread, -1, 1,
                                1);
    return;
  }
  if (profile->kind == PTX_MMA_F16) {
    ptx_emit_mma_f16_tile(fn, in, profile, &memory, group, thread, -1, 1, 1);
    return;
  }
  for (int m_tile = 0; m_tile < profile->m_tiles; m_tile++) {
    PtxMmaSparseAFragment sparse_a;
    PtxMmaSparseAFragment *sparse_a_ptr = NULL;
    if (ptx_mma_profile_is_sparse(profile)) {
      ptx_mma_prepare_sparse_f16_a(
          fn, in, &memory, group, thread, (unsigned)m_tile * 16u, &sparse_a);
      sparse_a_ptr = &sparse_a;
    }
    for (int n_tile = 0; n_tile < profile->n_tiles; n_tile++) {
      ptx_emit_mma_native_subtile(
          fn, in, profile, &memory, group, thread, sparse_a_ptr,
          (unsigned)m_tile * 16u, (unsigned)n_tile * 8u, -1, 1, 1);
    }
  }
}

static void ptx_emit_tensor_mma_native_chain_resident(
    PtxFn *fn, const IRInstruction *in, const PtxMmaProfile *profile,
    size_t tile_count, size_t per_tile, int tuple_budget,
    int estimated_peak) {
  int subtile_count = profile->m_tiles * profile->n_tiles;
  int accumulator_count = subtile_count * profile->accumulator_registers;
  int accumulator_base = fn->count[PC_F32];
  for (int i = 0; i < accumulator_count; i++) new_reg(fn, PC_F32);

  char lane[24], group[24], thread[24];
  reg_name(PC_B32, new_reg(fn, PC_B32), lane);
  reg_name(PC_B32, new_reg(fn, PC_B32), group);
  reg_name(PC_B32, new_reg(fn, PC_B32), thread);
  sb_printf(&fn->body,
            "\t// mtlc.tensor_chain resident native-mma %s tiles=%llu subtiles=%d tuple_peak=%d budget=%d\n",
            ptx_mma_kind_name(profile),
            (unsigned long long)tile_count, subtile_count, estimated_peak,
            tuple_budget);
  sb_printf(&fn->body, "\tmov.u32 %s, %%laneid;\n", lane);
  sb_printf(&fn->body, "\tshr.u32 %s, %s, 2;\n", group, lane);
  sb_printf(&fn->body, "\tand.b32 %s, %s, 3;\n", thread, lane);

  for (size_t tile = 0; tile < tile_count; tile++) {
    size_t base = tile * per_tile;
    PtxMmaTileMemory memory;
    if (!ptx_mma_prepare_tile_memory(fn, in, profile, base, per_tile,
                                     &memory)) {
      fn_error(fn,
               "PTX native resident chain has invalid pointers or strides");
      return;
    }
    if (profile->kind == PTX_MMA_S8_SCALED) {
      ptx_emit_mma_s8_scaled_tile(fn, in, profile, &memory, group, thread,
                                  accumulator_base, tile == 0,
                                  tile + 1 == tile_count);
      continue;
    }
    if (profile->kind == PTX_MMA_F16) {
      ptx_emit_mma_f16_tile(fn, in, profile, &memory, group, thread,
                            accumulator_base, tile == 0,
                            tile + 1 == tile_count);
      continue;
    }
    for (int m_tile = 0; m_tile < profile->m_tiles; m_tile++) {
      PtxMmaSparseAFragment sparse_a;
      PtxMmaSparseAFragment *sparse_a_ptr = NULL;
      if (ptx_mma_profile_is_sparse(profile)) {
        ptx_mma_prepare_sparse_f16_a(
            fn, in, &memory, group, thread, (unsigned)m_tile * 16u,
            &sparse_a);
        sparse_a_ptr = &sparse_a;
      }
      for (int n_tile = 0; n_tile < profile->n_tiles; n_tile++) {
        int subtile = m_tile * profile->n_tiles + n_tile;
        ptx_emit_mma_native_subtile(
            fn, in, profile, &memory, group, thread, sparse_a_ptr,
            (unsigned)m_tile * 16u, (unsigned)n_tile * 8u,
            accumulator_base + subtile * profile->accumulator_registers,
            tile == 0, tile + 1 == tile_count);
      }
    }
  }
}

static void ptx_emit_wmma_mma(PtxFn *fn, const MtlcTensorMmaDesc *desc,
                              const PtxWmmaProfile *profile,
                              const char *d, const char *a, const char *b,
                              const char *c) {
  const char *alayout = ptx_tensor_layout(desc->a_layout);
  const char *blayout = ptx_tensor_layout(desc->b_layout);
  if (profile->kind == PTX_WMMA_F16) {
    sb_printf(&fn->body,
              "\twmma.mma.sync.aligned.%s.%s.%s.%s.%s %s, %s, %s, %s;\n",
              profile->shape, alayout, blayout, profile->d_type,
              profile->c_type, d, a, b, c);
  } else if (profile->kind == PTX_WMMA_B1) {
    const char *op = desc->math_mode == MTLC_TENSOR_MATH_XOR_POPCOUNT
                         ? "xor"
                         : "and";
    sb_printf(&fn->body,
              "\twmma.mma.%s.popc.sync.aligned.%s.row.col.s32.b1.b1.s32 %s, %s, %s, %s;\n",
              op, profile->shape, d, a, b, c);
  } else if (profile->kind == PTX_WMMA_I8 ||
             profile->kind == PTX_WMMA_I4) {
    sb_printf(&fn->body,
              "\twmma.mma.sync.aligned.%s.%s.%s.s32.%s.%s.s32%s %s, %s, %s, %s;\n",
              profile->shape, alayout, blayout, profile->a_type,
              profile->b_type,
              desc->overflow == MTLC_TENSOR_OVERFLOW_SATURATE_FINITE
                  ? ".satfinite"
                  : "",
              d, a, b, c);
  } else if (profile->kind == PTX_WMMA_F64) {
    sb_printf(&fn->body,
              "\twmma.mma.sync.aligned.%s.%s.%s.f64.f64.f64.f64 %s, %s, %s, %s;\n",
              profile->shape, alayout, blayout, d, a, b, c);
  } else {
    sb_printf(&fn->body,
              "\twmma.mma.sync.aligned.%s.%s.%s.f32.%s.%s.f32 %s, %s, %s, %s;\n",
              profile->shape, alayout, blayout, profile->a_type,
              profile->b_type, d, a, b, c);
  }
}

static int ptx_tensor_stride_registers(PtxFn *fn, const IRInstruction *in,
                                       size_t base, size_t per_tile,
                                       char registers[4][24]);

static unsigned ptx_wmma_element_bytes(MtlcTensorElement element) {
  switch (element) {
  case MTLC_TENSOR_ELEMENT_FLOAT16:
  case MTLC_TENSOR_ELEMENT_BFLOAT16:
    return 2;
  case MTLC_TENSOR_ELEMENT_TFLOAT32:
  case MTLC_TENSOR_ELEMENT_FLOAT32:
  case MTLC_TENSOR_ELEMENT_INT32:
    return 4;
  case MTLC_TENSOR_ELEMENT_FLOAT64:
    return 8;
  case MTLC_TENSOR_ELEMENT_INT8:
  case MTLC_TENSOR_ELEMENT_UINT8:
  case MTLC_TENSOR_ELEMENT_FLOAT8_E4M3:
  case MTLC_TENSOR_ELEMENT_FLOAT8_E5M2:
    return 1;
  default:
    return 0;
  }
}

static int ptx_wmma_offset_pointer(PtxFn *fn, const char *base,
                                   const char *stride,
                                   MtlcTensorLayout layout,
                                   unsigned row, unsigned column,
                                   MtlcTensorElement element,
                                   char pointer[24]) {
  if (!fn || !base || !stride || !pointer) return 0;
  if (row == 0 && column == 0) {
    snprintf(pointer, 24, "%s", base);
    return 1;
  }
  unsigned bytes = ptx_wmma_element_bytes(element);
  if (bytes == 0 || (layout != MTLC_TENSOR_LAYOUT_ROW_MAJOR &&
                     layout != MTLC_TENSOR_LAYOUT_COLUMN_MAJOR))
    return 0;
  unsigned major = layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR ? row : column;
  unsigned minor = layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR ? column : row;
  reg_name(PC_B64, new_reg(fn, PC_B64), pointer);
  if (major == 0) {
    sb_printf(&fn->body, "\tadd.u64 %s, %s, %llu;\n", pointer, base,
              (unsigned long long)minor * bytes);
    return 1;
  }
  sb_printf(&fn->body, "\tmul.wide.u32 %s, %s, %u;\n", pointer, stride,
            major);
  if (minor != 0)
    sb_printf(&fn->body, "\tadd.u64 %s, %s, %u;\n", pointer, pointer,
              minor);
  if (bytes == 2)
    sb_printf(&fn->body, "\tshl.b64 %s, %s, 1;\n", pointer, pointer);
  else if (bytes == 4)
    sb_printf(&fn->body, "\tshl.b64 %s, %s, 2;\n", pointer, pointer);
  else if (bytes == 8)
    sb_printf(&fn->body, "\tshl.b64 %s, %s, 3;\n", pointer, pointer);
  sb_printf(&fn->body, "\tadd.u64 %s, %s, %s;\n", pointer, base, pointer);
  return 1;
}

static int ptx_emit_wmma_tiled_subtile(
    PtxFn *fn, const IRInstruction *in, const PtxWmmaProfile *profile,
    const char *c_base, const char *d_base,
    const char *c_space, const char *d_space, char strides[4][24],
    unsigned m_tile, unsigned n_tile, const char *a, const char *b,
    const char *c, const char *scratch_d, int accumulator_base,
    int load_accumulator, int store_accumulator, int c_zero_base) {
  unsigned row = m_tile * profile->tile_m;
  unsigned column = n_tile * profile->tile_n;
  int subtile = (int)(m_tile * (unsigned)profile->n_tiles + n_tile);
  char accumulator[256], cp[24], dp[24];
  if (accumulator_base >= 0) {
    ptx_reg_tuple_at(profile->d_class,
                     accumulator_base + subtile * profile->d_registers,
                     profile->d_registers, accumulator,
                     sizeof(accumulator));
  } else {
    snprintf(accumulator, sizeof(accumulator), "%s", scratch_d);
  }
  if (load_accumulator && c_zero_base >= 0) {
    for (int i = 0; i < profile->c_registers; i++) {
      char zero[24];
      reg_name(profile->c_class, c_zero_base + i, zero);
      sb_printf(&fn->body, "\tmov%s %s, %s;\n", cls_regtype(profile->c_class),
                zero, profile->c_class == PC_F32 ? "0f00000000" : "0");
    }
  } else if (load_accumulator) {
    if (!ptx_wmma_offset_pointer(fn, c_base, strides[2],
                                 IR_TENSOR_MMA(in).c_layout, row, column,
                                 IR_TENSOR_MMA(in).accumulator_element, cp)) {
      fn_error(fn, "PTX tiled WMMA cannot address the C subtile");
      return 0;
    }
    sb_printf(&fn->body,
              "\twmma.load.c.sync.aligned.%s%s.%s.%s %s, [%s], %s;\n",
              profile->shape, c_space,
              ptx_tensor_layout(IR_TENSOR_MMA(in).c_layout), profile->c_type,
              c, cp, strides[2]);
  }
  ptx_emit_wmma_mma(fn, &IR_TENSOR_MMA(in), profile, accumulator, a, b,
                    load_accumulator ? c : accumulator);
  if (store_accumulator) {
    if (!ptx_wmma_offset_pointer(fn, d_base, strides[3],
                                 IR_TENSOR_MMA(in).d_layout, row, column,
                                 IR_TENSOR_MMA(in).result_element, dp)) {
      fn_error(fn, "PTX tiled WMMA cannot address the D subtile");
      return 0;
    }
    sb_printf(&fn->body,
              "\twmma.store.d.sync.aligned.%s%s.%s.%s [%s], %s, %s;\n",
              profile->shape, d_space,
              ptx_tensor_layout(IR_TENSOR_MMA(in).d_layout), profile->d_type,
              dp, accumulator, strides[3]);
  }
  return 1;
}

static int ptx_emit_wmma_tiled_tile(
    PtxFn *fn, const IRInstruction *in, const PtxWmmaProfile *profile,
    size_t base, size_t per_tile, int accumulator_base,
    int load_accumulator, int store_accumulator) {
  if (!fn || !in || !profile || profile->m_tiles <= 0 ||
      profile->n_tiles <= 0 || base > in->argument_count ||
      per_tile > in->argument_count - base) {
    if (fn) fn_error(fn, "PTX tiled WMMA operand range is inconsistent");
    return 0;
  }
  PtxVal av = operand_desc(fn, &in->arguments[base]);
  PtxVal bv = operand_desc(fn, &in->arguments[base + 1]);
  int c_zero = ir_tensor_c_is_zero(&in->arguments[base + 2]);
  PtxVal cv = operand_desc(fn, &in->arguments[base + 2]);
  PtxVal dv = operand_desc(fn, &in->arguments[base + 3]);
  const char *spaces[4] = {ptx_wmma_space(av), ptx_wmma_space(bv),
                           c_zero ? "" : ptx_wmma_space(cv),
                           ptx_wmma_space(dv)};
  if (!av.is_ptr || !bv.is_ptr || (!c_zero && !cv.is_ptr) || !dv.is_ptr ||
      !spaces[0] || !spaces[1] || !spaces[2] || !spaces[3]) {
    fn_error(fn,
             "PTX tiled WMMA requires generic/global/workgroup tile pointers");
    return 0;
  }
  char pointers[4][24] = {{0}}, strides[4][24];
  for (size_t operand = 0; operand < 4; operand++)
    if (operand != 2 || !c_zero)
      use_as(fn, &in->arguments[base + operand], PC_B64, pointers[operand]);
  if (!ptx_tensor_stride_registers(fn, in, base, per_tile, strides)) {
    fn_error(fn, "PTX tiled WMMA has inconsistent stride operands");
    return 0;
  }

  char a[256], b[256], c[256] = {0}, scratch_d[256] = {0};
  int c_zero_base = -1;
  ptx_reg_tuple(fn, profile->a_class, profile->a_registers, a, sizeof(a));
  ptx_reg_tuple(fn, profile->b_class, profile->b_registers, b, sizeof(b));
  if (load_accumulator) {
    if (c_zero) c_zero_base = fn->count[profile->c_class];
    ptx_reg_tuple(fn, profile->c_class, profile->c_registers, c, sizeof(c));
  }
  if (accumulator_base < 0)
    ptx_reg_tuple(fn, profile->d_class, profile->d_registers, scratch_d,
                  sizeof(scratch_d));

  int reuse_a = profile->n_tiles >= profile->m_tiles;
  if (profile->m_tiles * profile->n_tiles > 1) {
    sb_printf(&fn->body,
              "\t// mtlc.tensor_mma tiled logical=m%un%uk%u physical=%s subtiles=%d reuse=%s\n",
              (unsigned)IR_TENSOR_MMA(in).m, (unsigned)IR_TENSOR_MMA(in).n,
              (unsigned)IR_TENSOR_MMA(in).k, profile->shape,
              profile->m_tiles * profile->n_tiles,
              reuse_a ? "A" : "B");
  }
  if (reuse_a) {
    for (int m = 0; m < profile->m_tiles; m++) {
      char ap[24];
      if (!ptx_wmma_offset_pointer(
              fn, pointers[0], strides[0], IR_TENSOR_MMA(in).a_layout,
              (unsigned)m * profile->tile_m, 0,
              IR_TENSOR_MMA(in).a_element, ap)) {
        fn_error(fn, "PTX tiled WMMA cannot address the A subtile");
        return 0;
      }
      sb_printf(&fn->body,
                "\twmma.load.a.sync.aligned.%s%s.%s.%s %s, [%s], %s;\n",
                profile->shape, spaces[0],
                ptx_tensor_layout(IR_TENSOR_MMA(in).a_layout), profile->a_type,
                a, ap, strides[0]);
      for (int n = 0; n < profile->n_tiles; n++) {
        char bp[24];
        if (!ptx_wmma_offset_pointer(
                fn, pointers[1], strides[1], IR_TENSOR_MMA(in).b_layout, 0,
                (unsigned)n * profile->tile_n,
                IR_TENSOR_MMA(in).b_element, bp)) {
          fn_error(fn, "PTX tiled WMMA cannot address the B subtile");
          return 0;
        }
        sb_printf(&fn->body,
                  "\twmma.load.b.sync.aligned.%s%s.%s.%s %s, [%s], %s;\n",
                  profile->shape, spaces[1],
                  ptx_tensor_layout(IR_TENSOR_MMA(in).b_layout),
                  profile->b_type, b, bp, strides[1]);
        if (!ptx_emit_wmma_tiled_subtile(
                fn, in, profile, pointers[2], pointers[3], spaces[2],
                spaces[3], strides, (unsigned)m, (unsigned)n, a, b, c,
                scratch_d, accumulator_base, load_accumulator,
                store_accumulator, c_zero_base))
          return 0;
      }
    }
  } else {
    for (int n = 0; n < profile->n_tiles; n++) {
      char bp[24];
      if (!ptx_wmma_offset_pointer(
              fn, pointers[1], strides[1], IR_TENSOR_MMA(in).b_layout, 0,
              (unsigned)n * profile->tile_n,
              IR_TENSOR_MMA(in).b_element, bp)) {
        fn_error(fn, "PTX tiled WMMA cannot address the B subtile");
        return 0;
      }
      sb_printf(&fn->body,
                "\twmma.load.b.sync.aligned.%s%s.%s.%s %s, [%s], %s;\n",
                profile->shape, spaces[1],
                ptx_tensor_layout(IR_TENSOR_MMA(in).b_layout), profile->b_type,
                b, bp, strides[1]);
      for (int m = 0; m < profile->m_tiles; m++) {
        char ap[24];
        if (!ptx_wmma_offset_pointer(
                fn, pointers[0], strides[0], IR_TENSOR_MMA(in).a_layout,
                (unsigned)m * profile->tile_m, 0,
                IR_TENSOR_MMA(in).a_element, ap)) {
          fn_error(fn, "PTX tiled WMMA cannot address the A subtile");
          return 0;
        }
        sb_printf(&fn->body,
                  "\twmma.load.a.sync.aligned.%s%s.%s.%s %s, [%s], %s;\n",
                  profile->shape, spaces[0],
                  ptx_tensor_layout(IR_TENSOR_MMA(in).a_layout),
                  profile->a_type, a, ap, strides[0]);
        if (!ptx_emit_wmma_tiled_subtile(
                fn, in, profile, pointers[2], pointers[3], spaces[2],
                spaces[3], strides, (unsigned)m, (unsigned)n, a, b, c,
                scratch_d, accumulator_base, load_accumulator,
                store_accumulator, c_zero_base))
          return 0;
      }
    }
  }
  return !fn->error;
}

static int ptx_emit_wmma_tiled_store(
    PtxFn *fn, const IRInstruction *in, const PtxWmmaProfile *profile,
    size_t base, size_t per_tile, int accumulator_base) {
  PtxVal dv = operand_desc(fn, &in->arguments[base + 3]);
  const char *dspace = ptx_wmma_space(dv);
  char dp_base[24], strides[4][24];
  if (!dv.is_ptr || !dspace || accumulator_base < 0) {
    fn_error(fn, "PTX tiled WMMA commit requires a writable D tile");
    return 0;
  }
  use_as(fn, &in->arguments[base + 3], PC_B64, dp_base);
  if (!ptx_tensor_stride_registers(fn, in, base, per_tile, strides)) {
    fn_error(fn, "PTX tiled WMMA commit has inconsistent strides");
    return 0;
  }
  for (int m = 0; m < profile->m_tiles; m++) {
    for (int n = 0; n < profile->n_tiles; n++) {
      int subtile = m * profile->n_tiles + n;
      char dp[24], accumulator[256];
      if (!ptx_wmma_offset_pointer(
              fn, dp_base, strides[3], IR_TENSOR_MMA(in).d_layout,
              (unsigned)m * profile->tile_m,
              (unsigned)n * profile->tile_n,
              IR_TENSOR_MMA(in).result_element, dp)) {
        fn_error(fn, "PTX tiled WMMA commit cannot address the D subtile");
        return 0;
      }
      ptx_reg_tuple_at(profile->d_class,
                       accumulator_base + subtile * profile->d_registers,
                       profile->d_registers, accumulator,
                       sizeof(accumulator));
      sb_printf(&fn->body,
                "\twmma.store.d.sync.aligned.%s%s.%s.%s [%s], %s, %s;\n",
                profile->shape, dspace,
                ptx_tensor_layout(IR_TENSOR_MMA(in).d_layout),
                profile->d_type, dp, accumulator, strides[3]);
    }
  }
  return !fn->error;
}

static void ptx_report_tensor(PtxFn *fn, const IRInstruction *in,
                              IRDeviceVerdict verdict, const char *headline,
                              const char *reason, const char *fix,
                              const char *code) {
  char subject[64];
  if (!fn->reported) return;
  snprintf(subject, sizeof(subject), "%s m%un%uk%u",
           in->op == IR_OP_TENSOR_MATMUL ? "tensor_matmul" : "tensor_mma",
           (unsigned)IR_TENSOR_MMA(in).m, (unsigned)IR_TENSOR_MMA(in).n,
           (unsigned)IR_TENSOR_MMA(in).k);
  ir_explain_device_note(fn->function->name, in->location.line, "tensor",
                         subject, verdict, headline, reason, fix, code);
}

static void ptx_report_tensor_instruction(PtxFn *fn, const IRInstruction *in,
                                          const char *instruction,
                                          const char *shape, const char *kind,
                                          int subtiles) {
  char headline[160];
  if (!fn->reported) return;
  if (subtiles > 1) {
    snprintf(headline, sizeof(headline), "%s %s %s, %d tiles a step",
             instruction, shape, kind, subtiles);
  } else {
    snprintf(headline, sizeof(headline), "%s %s %s", instruction, shape, kind);
  }
  ptx_report_tensor(fn, in, IR_DEVICE_GOOD, headline, NULL, NULL,
                    "tensor-instruction");
}

static void ptx_report_tensor_resident(PtxFn *fn, const IRInstruction *in,
                                       const char *what, int peak, int budget) {
  char headline[160];
  if (!fn->reported) return;
  snprintf(headline, sizeof(headline),
           "the accumulator stays in registers across %s, %d of a budget of "
           "%d",
           what, peak, budget);
  ptx_report_tensor(fn, in, IR_DEVICE_GOOD, headline, NULL, NULL,
                    "tensor-resident");
}

static void ptx_report_tensor_replay(PtxFn *fn, const IRInstruction *in,
                                     const char *what, int peak, int budget) {
  char headline[160], reason[200], fix[120];
  if (!fn->reported) return;
  snprintf(headline, sizeof(headline),
           "every step of %s loads and stores the accumulator", what);
  snprintf(reason, sizeof(reason),
           "keeping it in registers takes %d of them, and the tensor register "
           "budget is %d",
           peak, budget);
  snprintf(fix, sizeof(fix), "raise the budget with --gpu-tensor-tuple-budget=%d",
           peak);
  ptx_report_tensor(fn, in, IR_DEVICE_MISS, headline, reason, fix,
                    "tensor-register-budget");
}

static void ptx_report_tensor_matmul(PtxFn *fn, const IRInstruction *in,
                                     int have_native, int direct,
                                     const PtxWmmaProfile *wmma,
                                     const PtxMmaProfile *mma, int native_peak,
                                     const char *native_reason) {
  char headline[200], reason[320], fix[120];
  int budget = ptx_tensor_tuple_budget(fn);
  if (!fn->reported) return;
  if (have_native) {
    snprintf(headline, sizeof(headline),
             "full tiles through %s %s %s, edge tiles one element at a time",
             direct ? "mma.sync" : "wmma.mma",
             direct ? mma->shape : wmma->shape,
             direct ? ptx_mma_kind_name(mma) : wmma->a_type);
    ptx_report_tensor(fn, in, IR_DEVICE_GOOD, headline, NULL, NULL,
                      "tensor-instruction");
    return;
  }
  if (native_peak > budget) {
    snprintf(reason, sizeof(reason),
             "the tensor instruction's accumulator takes %d registers, and "
             "the tensor register budget is %d",
             native_peak, budget);
    snprintf(fix, sizeof(fix),
             "raise the budget with --gpu-tensor-tuple-budget=%d", native_peak);
  } else {
    snprintf(reason, sizeof(reason), "no tensor instruction fits: %s",
             native_reason);
  }
  ptx_report_tensor(fn, in, IR_DEVICE_MISS,
                    "computed one element at a time by the whole warp", reason,
                    native_peak > budget ? fix : NULL, "tensor-scalar-replay");
}

static const char *ptx_residency_scope_words(const IRInstruction *in) {
  const char *name = ptx_tensor_residency_name(in->tensor_residency_scope);
  if (name && strstr(name, "loop")) return "the loop";
  if (name && strstr(name, "pipeline")) return "the pipeline";
  return "the region";
}

static void ptx_emit_tensor_mma_single(PtxFn *fn, const IRInstruction *in) {
  if (ptx_tensor_uses_direct_mma(&IR_TENSOR_MMA(in))) {
    PtxMmaProfile native_profile;
    char native_reason[256];
    if (!ptx_select_mma_profile(fn, &IR_TENSOR_MMA(in), &native_profile,
                                native_reason, sizeof(native_reason))) {
      fn_error(fn, "PTX native tensor MMA cannot lower m%un%uk%u: %s",
               (unsigned)IR_TENSOR_MMA(in).m, (unsigned)IR_TENSOR_MMA(in).n,
               (unsigned)IR_TENSOR_MMA(in).k, native_reason);
      return;
    }
    ptx_report_tensor_instruction(
        fn, in, "mma.sync", native_profile.shape,
        ptx_mma_kind_name(&native_profile),
        native_profile.m_tiles * native_profile.n_tiles);
    ptx_emit_tensor_mma_native_single(fn, in, &native_profile);
    return;
  }
  PtxWmmaProfile profile;
  char reason[256];
  if (!ptx_select_wmma_profile(fn, &IR_TENSOR_MMA(in), &profile, reason,
                               sizeof(reason))) {
    fn_error(fn, "PTX tensor MMA cannot lower m%un%uk%u: %s",
             (unsigned)IR_TENSOR_MMA(in).m, (unsigned)IR_TENSOR_MMA(in).n,
             (unsigned)IR_TENSOR_MMA(in).k, reason);
    return;
  }
  size_t expected_operands = ir_tensor_mma_operand_count(&IR_TENSOR_MMA(in));
  if (!expected_operands || in->argument_count != expected_operands) {
    fn_error(fn, "PTX stable WMMA tensor operand count is inconsistent");
    return;
  }
  ptx_report_tensor_instruction(fn, in, "wmma.mma", profile.shape,
                                profile.a_type,
                                profile.m_tiles * profile.n_tiles);
  (void)ptx_emit_wmma_tiled_tile(fn, in, &profile, 0,
                                  expected_operands, -1, 1, 1);
}

static int ptx_tensor_stride_registers(PtxFn *fn, const IRInstruction *in,
                                       size_t base, size_t per_tile,
                                       char registers[4][24]) {
  uint32_t static_strides[4] = {
      IR_TENSOR_MMA(in).a_leading_dimension,
      IR_TENSOR_MMA(in).b_leading_dimension,
      IR_TENSOR_MMA(in).c_leading_dimension,
      IR_TENSOR_MMA(in).d_leading_dimension};
  size_t stride_argument =
      base + 4u +
      (IR_TENSOR_MMA(in).sparsity != MTLC_TENSOR_SPARSITY_DENSE ? 1u : 0u) +
      (IR_TENSOR_MMA(in).a_scale_mode != MTLC_TENSOR_SCALE_NONE ? 1u : 0u) +
      (IR_TENSOR_MMA(in).b_scale_mode != MTLC_TENSOR_SCALE_NONE ? 1u : 0u);
  for (size_t stride = 0; stride < 4; stride++) {
    if (static_strides[stride] == 0) {
      if (stride_argument >= base + per_tile) return 0;
      use_as(fn, &in->arguments[stride_argument++], PC_B32,
             registers[stride]);
    } else {
      reg_name(PC_B32, new_reg(fn, PC_B32), registers[stride]);
      sb_printf(&fn->body, "\tmov.u32 %s, %u;\n", registers[stride],
                static_strides[stride]);
    }
  }
  return stride_argument ==
         base + per_tile -
             (IR_TENSOR_MMA(in).c_scale_mode != MTLC_TENSOR_SCALE_NONE ? 1u
                                                                       : 0u);
}

static void ptx_emit_tensor_mma_chain_resident(
    PtxFn *fn, const IRInstruction *in, const PtxWmmaProfile *profile,
    size_t tile_count, size_t per_tile, int tuple_budget,
    int estimated_peak) {
  int subtile_count = profile->m_tiles * profile->n_tiles;
  int accumulator_count = subtile_count * profile->d_registers;
  int accumulator_base = fn->count[profile->d_class];
  for (int i = 0; i < accumulator_count; i++)
    new_reg(fn, profile->d_class);
  if (subtile_count == 1)
    sb_printf(&fn->body,
              "\t// mtlc.tensor_chain resident tiles=%llu tuple_peak=%d budget=%d\n",
              (unsigned long long)tile_count, estimated_peak, tuple_budget);
  else
    sb_printf(&fn->body,
              "\t// mtlc.tensor_chain resident tiles=%llu subtiles=%d tuple_peak=%d budget=%d\n",
              (unsigned long long)tile_count, subtile_count, estimated_peak,
              tuple_budget);
  for (size_t tile = 0; tile < tile_count; tile++) {
    if (!ptx_emit_wmma_tiled_tile(
            fn, in, profile, tile * per_tile, per_tile, accumulator_base,
            tile == 0, tile + 1 == tile_count))
      return;
  }
}

static void ptx_emit_tensor_residency_mma_native(
    PtxFn *fn, const IRInstruction *in, const PtxMmaProfile *profile,
    const char *residency_name, size_t per_tile) {
  int subtile_count = profile->m_tiles * profile->n_tiles;
  int accumulator_count = subtile_count * profile->accumulator_registers;
  int estimated_peak = accumulator_count + profile->a_registers +
                       profile->b_registers;
  int tuple_budget = ptx_tensor_tuple_budget(fn);
  PtxTensorResidency *group =
      ptx_tensor_residency_find(fn, in->tensor_residency_id);
  if (in->tensor_residency_role == IR_TENSOR_RESIDENCY_START) {
    if (group) {
      fn_error(fn, "PTX tensor residency group %u has multiple starts",
               in->tensor_residency_id);
      return;
    }
    group = ptx_tensor_residency_add(fn, in->tensor_residency_id);
    if (!group) {
      fn_error(fn, "PTX could not allocate tensor residency group %u",
               in->tensor_residency_id);
      return;
    }
    group->scope = in->tensor_residency_scope;
    group->resident = estimated_peak <= tuple_budget;
    group->tuple_peak = estimated_peak;
    if (!group->resident) {
      sb_printf(
          &fn->body,
          "\t// mtlc.%s replay native-mma %s group=%u tuple_peak=%d budget=%d\n",
          residency_name, ptx_mma_kind_name(profile), group->id,
          estimated_peak, tuple_budget);
      ptx_report_tensor_replay(fn, in, ptx_residency_scope_words(in),
                               estimated_peak, tuple_budget);
      ptx_emit_tensor_mma_single(fn, in);
      return;
    }
    ptx_report_tensor_instruction(fn, in, "mma.sync", profile->shape,
                                  ptx_mma_kind_name(profile), subtile_count);
    ptx_report_tensor_resident(fn, in, ptx_residency_scope_words(in),
                               estimated_peak, tuple_budget);
    group->accumulator_class = PC_F32;
    group->accumulator_base = fn->count[PC_F32];
    group->accumulator_count = accumulator_count;
    for (int i = 0; i < accumulator_count; i++) new_reg(fn, PC_F32);
    sb_printf(
        &fn->body,
        "\t// mtlc.%s resident native-mma %s group=%u subtiles=%d tuple_peak=%d budget=%d\n",
        residency_name, ptx_mma_kind_name(profile), group->id,
        subtile_count, estimated_peak, tuple_budget);
  } else if (!group) {
    fn_error(fn, "PTX tensor residency update %u precedes its start",
             in->tensor_residency_id);
    return;
  } else if (group->scope != in->tensor_residency_scope) {
    fn_error(fn, "PTX tensor residency group %u changed scope", group->id);
    return;
  } else if (!group->resident) {
    ptx_emit_tensor_mma_single(fn, in);
    return;
  }
  if (group->accumulator_class != PC_F32 ||
      group->accumulator_count != accumulator_count) {
    fn_error(fn, "PTX tensor residency group %u changed native MMA profile",
             group->id);
    return;
  }

  PtxMmaTileMemory memory;
  if (!ptx_mma_prepare_tile_memory(fn, in, profile, 0, per_tile, &memory)) {
    fn_error(fn,
             "PTX native tensor residency has invalid pointers or strides");
    return;
  }
  char lane[24], lane_group[24], thread[24];
  reg_name(PC_B32, new_reg(fn, PC_B32), lane);
  reg_name(PC_B32, new_reg(fn, PC_B32), lane_group);
  reg_name(PC_B32, new_reg(fn, PC_B32), thread);
  sb_printf(&fn->body, "\tmov.u32 %s, %%laneid;\n", lane);
  sb_printf(&fn->body, "\tshr.u32 %s, %s, 2;\n", lane_group, lane);
  sb_printf(&fn->body, "\tand.b32 %s, %s, 3;\n", thread, lane);
  if (profile->kind == PTX_MMA_S8_SCALED) {
    ptx_emit_mma_s8_scaled_tile(
        fn, in, profile, &memory, lane_group, thread, group->accumulator_base,
        in->tensor_residency_role == IR_TENSOR_RESIDENCY_START, 0);
    return;
  }
  if (profile->kind == PTX_MMA_F16) {
    ptx_emit_mma_f16_tile(
        fn, in, profile, &memory, lane_group, thread, group->accumulator_base,
        in->tensor_residency_role == IR_TENSOR_RESIDENCY_START, 0);
    return;
  }
  for (int m_tile = 0; m_tile < profile->m_tiles; m_tile++) {
    PtxMmaSparseAFragment sparse_a;
    PtxMmaSparseAFragment *sparse_a_ptr = NULL;
    if (ptx_mma_profile_is_sparse(profile)) {
      ptx_mma_prepare_sparse_f16_a(
          fn, in, &memory, lane_group, thread, (unsigned)m_tile * 16u,
          &sparse_a);
      sparse_a_ptr = &sparse_a;
    }
    for (int n_tile = 0; n_tile < profile->n_tiles; n_tile++) {
      int subtile = m_tile * profile->n_tiles + n_tile;
      ptx_emit_mma_native_subtile(
          fn, in, profile, &memory, lane_group, thread, sparse_a_ptr,
          (unsigned)m_tile * 16u, (unsigned)n_tile * 8u,
          group->accumulator_base +
              subtile * profile->accumulator_registers,
          in->tensor_residency_role == IR_TENSOR_RESIDENCY_START, 0);
    }
  }
}

static void ptx_emit_tensor_residency_commit_native(
    PtxFn *fn, const IRInstruction *in, const PtxMmaProfile *profile,
    PtxTensorResidency *group, size_t per_tile) {
  int accumulator_count = profile->m_tiles * profile->n_tiles *
                          profile->accumulator_registers;
  if (group->accumulator_class != PC_F32 ||
      group->accumulator_count != accumulator_count) {
    fn_error(fn, "PTX tensor residency commit changed native MMA profile");
    return;
  }
  PtxVal dv = operand_desc(fn, &in->arguments[3]);
  const char *dspace = ptx_wmma_space(dv);
  if (!dv.is_ptr || !dspace) {
    fn_error(fn,
             "PTX native tensor residency commit requires a writable tile pointer");
    return;
  }
  char dp[24], strides[4][24];
  use_as(fn, &in->arguments[3], PC_B64, dp);
  if (!ptx_tensor_stride_registers(fn, in, 0, per_tile, strides)) {
    fn_error(fn, "PTX native tensor residency commit has inconsistent strides");
    return;
  }
  char lane[24], lane_group[24], thread[24];
  reg_name(PC_B32, new_reg(fn, PC_B32), lane);
  reg_name(PC_B32, new_reg(fn, PC_B32), lane_group);
  reg_name(PC_B32, new_reg(fn, PC_B32), thread);
  sb_printf(&fn->body, "\tmov.u32 %s, %%laneid;\n", lane);
  sb_printf(&fn->body, "\tshr.u32 %s, %s, 2;\n", lane_group, lane);
  sb_printf(&fn->body, "\tand.b32 %s, %s, 3;\n", thread, lane);
  for (int m_tile = 0; m_tile < profile->m_tiles; m_tile++) {
    for (int n_tile = 0; n_tile < profile->n_tiles; n_tile++) {
      int subtile = m_tile * profile->n_tiles + n_tile;
      ptx_mma_store_f32_accumulator_subtile(
          fn, in, profile, dp, dspace, strides[3], lane_group, thread,
          (unsigned)m_tile * 16u, (unsigned)n_tile * 8u,
          group->accumulator_base +
              subtile * profile->accumulator_registers);
    }
  }
}

static void ptx_emit_tensor_residency_mma(PtxFn *fn,
                                          const IRInstruction *in) {
  const char *residency_name =
      in ? ptx_tensor_residency_name(in->tensor_residency_scope) : NULL;
  if (!fn || !in || in->tensor_residency_id == 0 ||
      !residency_name ||
      (in->tensor_residency_role != IR_TENSOR_RESIDENCY_START &&
       in->tensor_residency_role != IR_TENSOR_RESIDENCY_UPDATE)) {
    fn_error(fn, "PTX received an invalid tensor residency MMA");
    return;
  }
  size_t per_tile = ir_tensor_mma_operand_count(&IR_TENSOR_MMA(in));
  if (!per_tile || in->argument_count != per_tile) {
    fn_error(fn, "PTX tensor residency operand count is inconsistent");
    return;
  }
  if (ptx_tensor_uses_direct_mma(&IR_TENSOR_MMA(in))) {
    PtxMmaProfile native_profile;
    char native_reason[256];
    if (!ptx_select_mma_profile(fn, &IR_TENSOR_MMA(in), &native_profile,
                                native_reason, sizeof(native_reason))) {
      fn_error(fn, "PTX native tensor residency cannot lower m%un%uk%u: %s",
               (unsigned)IR_TENSOR_MMA(in).m,
               (unsigned)IR_TENSOR_MMA(in).n,
               (unsigned)IR_TENSOR_MMA(in).k, native_reason);
      return;
    }
    ptx_emit_tensor_residency_mma_native(
        fn, in, &native_profile, residency_name, per_tile);
    return;
  }
  PtxWmmaProfile profile;
  char reason[256];
  if (!ptx_select_wmma_profile(fn, &IR_TENSOR_MMA(in), &profile, reason,
                               sizeof(reason))) {
    fn_error(fn, "PTX tensor residency cannot lower m%un%uk%u: %s",
             (unsigned)IR_TENSOR_MMA(in).m, (unsigned)IR_TENSOR_MMA(in).n,
             (unsigned)IR_TENSOR_MMA(in).k, reason);
    return;
  }
  int subtile_count = profile.m_tiles * profile.n_tiles;
  int accumulator_count = subtile_count * profile.d_registers;
  int estimated_peak = accumulator_count + profile.a_registers +
                       profile.b_registers + profile.c_registers;
  int tuple_budget = ptx_tensor_tuple_budget(fn);
  PtxTensorResidency *group = ptx_tensor_residency_find(
      fn, in->tensor_residency_id);
  if (in->tensor_residency_role == IR_TENSOR_RESIDENCY_START) {
    if (group) {
      fn_error(fn, "PTX tensor residency group %u has multiple starts",
               in->tensor_residency_id);
      return;
    }
    group = ptx_tensor_residency_add(fn, in->tensor_residency_id);
    if (!group) {
      fn_error(fn, "PTX could not allocate tensor residency group %u",
               in->tensor_residency_id);
      return;
    }
    group->scope = in->tensor_residency_scope;
    group->resident = estimated_peak <= tuple_budget;
    group->tuple_peak = estimated_peak;
    if (!group->resident) {
      sb_printf(&fn->body,
                "\t// mtlc.%s replay group=%u tuple_peak=%d budget=%d\n",
                residency_name, group->id, estimated_peak, tuple_budget);
      ptx_report_tensor_replay(fn, in, ptx_residency_scope_words(in),
                               estimated_peak, tuple_budget);
      ptx_emit_tensor_mma_single(fn, in);
      return;
    }
    ptx_report_tensor_instruction(fn, in, "wmma.mma", profile.shape,
                                  profile.a_type, subtile_count);
    ptx_report_tensor_resident(fn, in, ptx_residency_scope_words(in),
                               estimated_peak, tuple_budget);
    group->accumulator_class = profile.d_class;
    group->accumulator_base = fn->count[profile.d_class];
    group->accumulator_count = accumulator_count;
    for (int i = 0; i < accumulator_count; i++)
      new_reg(fn, profile.d_class);
    if (subtile_count == 1)
      sb_printf(&fn->body,
                "\t// mtlc.%s resident group=%u tuple_peak=%d budget=%d\n",
                residency_name, group->id, estimated_peak, tuple_budget);
    else
      sb_printf(
          &fn->body,
          "\t// mtlc.%s resident group=%u subtiles=%d tuple_peak=%d budget=%d\n",
          residency_name, group->id, subtile_count, estimated_peak,
          tuple_budget);
  } else if (!group) {
    fn_error(fn, "PTX tensor residency update %u precedes its start",
             in->tensor_residency_id);
    return;
  } else if (group->scope != in->tensor_residency_scope) {
    fn_error(fn, "PTX tensor residency group %u changed scope", group->id);
    return;
  } else if (!group->resident) {
    ptx_emit_tensor_mma_single(fn, in);
    return;
  }

  if (group->accumulator_class != profile.d_class ||
      group->accumulator_count != accumulator_count) {
    fn_error(fn, "PTX tensor residency group %u changed accumulator profile",
             group->id);
    return;
  }
  (void)ptx_emit_wmma_tiled_tile(
      fn, in, &profile, 0, per_tile, group->accumulator_base,
      in->tensor_residency_role == IR_TENSOR_RESIDENCY_START, 0);
}

static void ptx_emit_tensor_residency_commit(PtxFn *fn,
                                             const IRInstruction *in) {
  if (!fn || !in ||
      in->tensor_residency_role != IR_TENSOR_RESIDENCY_COMMIT ||
      in->tensor_residency_id == 0 ||
      !ptx_tensor_residency_name(in->tensor_residency_scope)) {
    fn_error(fn, "PTX received an invalid tensor residency commit");
    return;
  }
  PtxTensorResidency *group = ptx_tensor_residency_find(
      fn, in->tensor_residency_id);
  if (!group) {
    fn_error(fn, "PTX tensor residency commit %u has no start",
             in->tensor_residency_id);
    return;
  }
  if (group->scope != in->tensor_residency_scope) {
    fn_error(fn, "PTX tensor residency commit %u changed scope", group->id);
    return;
  }
  if (!group->resident) return;
  size_t per_tile = ir_tensor_mma_operand_count(&IR_TENSOR_MMA(in));
  if (ptx_tensor_uses_direct_mma(&IR_TENSOR_MMA(in))) {
    PtxMmaProfile native_profile;
    char native_reason[256];
    if (!per_tile || in->argument_count != per_tile ||
        !ptx_select_mma_profile(fn, &IR_TENSOR_MMA(in), &native_profile,
                                native_reason, sizeof(native_reason))) {
      fn_error(fn,
               "PTX native tensor residency commit has an invalid profile");
      return;
    }
    ptx_emit_tensor_residency_commit_native(
        fn, in, &native_profile, group, per_tile);
    return;
  }
  PtxWmmaProfile profile;
  char reason[256];
  if (!per_tile || in->argument_count != per_tile ||
      !ptx_select_wmma_profile(fn, &IR_TENSOR_MMA(in), &profile, reason,
                               sizeof(reason))) {
    fn_error(fn, "PTX tensor residency commit has an invalid profile");
    return;
  }
  int accumulator_count = profile.m_tiles * profile.n_tiles *
                          profile.d_registers;
  if (group->accumulator_class != profile.d_class ||
      group->accumulator_count != accumulator_count) {
    fn_error(fn, "PTX tensor residency commit changed accumulator profile");
    return;
  }
  (void)ptx_emit_wmma_tiled_store(fn, in, &profile, 0, per_tile,
                                   group->accumulator_base);
}

static void ptx_tensor_epilogue_load(PtxFn *fn, MtlcTensorElement element,
                                     const char *space,
                                     const char *address,
                                     const char *value) {
  if (element == MTLC_TENSOR_ELEMENT_FLOAT16 ||
      element == MTLC_TENSOR_ELEMENT_BFLOAT16) {
    char storage[24];
    reg_name(PC_B16, new_reg(fn, PC_B16), storage);
    sb_printf(&fn->body, "\tld%s.b16 %s, [%s];\n", space, storage,
              address);
    sb_printf(&fn->body, "\tcvt.f32.%s %s, %s;\n",
              element == MTLC_TENSOR_ELEMENT_FLOAT16 ? "f16" : "bf16",
              value, storage);
  } else {
    sb_printf(&fn->body, "\tld%s.%s %s, [%s];\n", space,
              element == MTLC_TENSOR_ELEMENT_FLOAT64 ? "f64" : "f32",
              value, address);
  }
}

static void ptx_tensor_epilogue_store(PtxFn *fn, MtlcTensorElement element,
                                      const char *space,
                                      const char *address,
                                      const char *value) {
  if (element == MTLC_TENSOR_ELEMENT_FLOAT16 ||
      element == MTLC_TENSOR_ELEMENT_BFLOAT16) {
    char storage[24];
    reg_name(PC_B16, new_reg(fn, PC_B16), storage);
    sb_printf(&fn->body, "\tcvt.rn.%s.f32 %s, %s;\n",
              element == MTLC_TENSOR_ELEMENT_FLOAT16 ? "f16" : "bf16",
              storage, value);
    sb_printf(&fn->body, "\tst%s.b16 [%s], %s;\n", space, address,
              storage);
  } else {
    sb_printf(&fn->body, "\tst%s.%s [%s], %s;\n", space,
              element == MTLC_TENSOR_ELEMENT_FLOAT64 ? "f64" : "f32",
              address, value);
  }
}

static void ptx_tensor_epilogue_address(
    PtxFn *fn, const char *base, const char *row, const char *column,
    const char *leading_dimension, MtlcTensorLayout layout,
    unsigned element_bytes, char address[24]) {
  char linear[24], byte_offset[24];
  reg_name(PC_B32, new_reg(fn, PC_B32), linear);
  if (layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR) {
    sb_printf(&fn->body, "\tmul.lo.u32 %s, %s, %s;\n", linear, row,
              leading_dimension);
    sb_printf(&fn->body, "\tadd.u32 %s, %s, %s;\n", linear, linear,
              column);
  } else {
    sb_printf(&fn->body, "\tmul.lo.u32 %s, %s, %s;\n", linear, column,
              leading_dimension);
    sb_printf(&fn->body, "\tadd.u32 %s, %s, %s;\n", linear, linear, row);
  }
  reg_name(PC_B64, new_reg(fn, PC_B64), byte_offset);
  sb_printf(&fn->body, "\tmul.wide.u32 %s, %s, %u;\n", byte_offset,
            linear, element_bytes);
  reg_name(PC_B64, new_reg(fn, PC_B64), address);
  sb_printf(&fn->body, "\tadd.u64 %s, %s, %s;\n", address, base,
            byte_offset);
}

static void ptx_emit_tensor_epilogue(PtxFn *fn,
                                     const IRInstruction *in) {
  if (!fn || !in || in->op != IR_OP_TENSOR_EPILOGUE ||
      !ir_tensor_epilogue_desc_valid(&IR_TENSOR_EPILOGUE(in))) {
    if (fn) fn_error(fn, "PTX received an invalid tensor epilogue");
    return;
  }
  const MtlcTensorEpilogueDesc *desc = &IR_TENSOR_EPILOGUE(in);
  size_t expected = ir_tensor_epilogue_operand_count(desc);
  if (!expected || in->argument_count != expected) {
    fn_error(fn, "PTX tensor epilogue has an invalid operand count");
    return;
  }
  if (desc->element == MTLC_TENSOR_ELEMENT_FLOAT16 &&
      (fn->target_arch < 53 || fn->isa_major < 4 ||
       (fn->isa_major == 4 && fn->isa_minor < 2))) {
    fn_error(fn,
             "PTX tensor epilogue f16 conversion requires PTX 4.2 and sm_53");
    return;
  }
  if (desc->element == MTLC_TENSOR_ELEMENT_BFLOAT16 &&
      (fn->target_arch < 80 || fn->isa_major < 7 ||
       (fn->isa_major == 7 && fn->isa_minor < 1))) {
    fn_error(fn,
             "PTX tensor epilogue bf16 conversion requires PTX 7.1 and sm_80");
    return;
  }
  if (desc->scope == MTLC_MEMORY_SCOPE_SUBGROUP &&
      (fn->target_arch < 70 || fn->isa_major < 6)) {
    fn_error(fn,
             "PTX subgroup tensor epilogue ordering requires PTX 6.0 and sm_70");
    return;
  }

  size_t argument = 0;
  PtxVal destination_desc = operand_desc(fn, &in->arguments[argument]);
  const char *destination_space = ptx_wmma_space(destination_desc);
  char destination[24];
  if (!destination_desc.is_ptr || !destination_space) {
    fn_error(fn,
             "PTX tensor epilogue destination must be a generic, global, or workgroup pointer");
    return;
  }
  use_as(fn, &in->arguments[argument++], PC_B64, destination);

  const char *bias_space = NULL;
  char bias[24] = {0};
  if (desc->bias_mode != MTLC_TENSOR_BIAS_NONE) {
    PtxVal bias_desc = operand_desc(fn, &in->arguments[argument]);
    bias_space = ptx_wmma_space(bias_desc);
    if (!bias_desc.is_ptr || !bias_space) {
      fn_error(fn,
               "PTX tensor epilogue bias must be a generic, global, or workgroup pointer");
      return;
    }
    use_as(fn, &in->arguments[argument++], PC_B64, bias);
  }

  PtxClass compute_class = desc->element == MTLC_TENSOR_ELEMENT_FLOAT64
                               ? PC_F64
                               : PC_F32;
  const char *compute_type = compute_class == PC_F64 ? "f64" : "f32";
  char alpha[24] = {0}, beta[24] = {0};
  char clamp_minimum[24] = {0}, clamp_maximum[24] = {0};
  if (desc->scale_output)
    use_as(fn, &in->arguments[argument++], compute_class, alpha);
  if (desc->scale_bias)
    use_as(fn, &in->arguments[argument++], compute_class, beta);
  if (desc->activation == MTLC_TENSOR_ACTIVATION_CLAMP) {
    use_as(fn, &in->arguments[argument++], compute_class, clamp_minimum);
    use_as(fn, &in->arguments[argument++], compute_class, clamp_maximum);
  }

  char leading_dimension[24], bias_leading_dimension[24] = {0};
  if (desc->leading_dimension == 0) {
    use_as(fn, &in->arguments[argument++], PC_B32, leading_dimension);
  } else {
    reg_name(PC_B32, new_reg(fn, PC_B32), leading_dimension);
    sb_printf(&fn->body, "\tmov.u32 %s, %u;\n", leading_dimension,
              desc->leading_dimension);
  }
  if (desc->bias_mode == MTLC_TENSOR_BIAS_MATRIX) {
    if (desc->bias_leading_dimension == 0) {
      use_as(fn, &in->arguments[argument++], PC_B32,
             bias_leading_dimension);
    } else {
      reg_name(PC_B32, new_reg(fn, PC_B32), bias_leading_dimension);
      sb_printf(&fn->body, "\tmov.u32 %s, %u;\n", bias_leading_dimension,
                desc->bias_leading_dimension);
    }
  }
  if (argument != expected) {
    fn_error(fn, "PTX tensor epilogue operand decoding is inconsistent");
    return;
  }

  char linear[24], participants[24];
  char row[24], column[24];
  reg_name(PC_B32, new_reg(fn, PC_B32), linear);
  reg_name(PC_B32, new_reg(fn, PC_B32), participants);
  reg_name(PC_B32, new_reg(fn, PC_B32), row);
  reg_name(PC_B32, new_reg(fn, PC_B32), column);
  char mask[24] = {0};
  if (desc->scope == MTLC_MEMORY_SCOPE_SUBGROUP) {
    char below[24], active_below[24];
    reg_name(PC_B32, new_reg(fn, PC_B32), mask);
    reg_name(PC_B32, new_reg(fn, PC_B32), below);
    reg_name(PC_B32, new_reg(fn, PC_B32), active_below);
    sb_printf(&fn->body,
              "\tactivemask.b32 %s;\n"
              "\tmov.u32 %s, %%lanemask_lt;\n"
              "\tand.b32 %s, %s, %s;\n"
              "\tpopc.b32 %s, %s;\n"
              "\tpopc.b32 %s, %s;\n"
              "\tbar.warp.sync %s;\n",
              mask, below, active_below, mask, below, linear, active_below,
              participants, mask, mask);
  } else {
    char tid_x[24], tid_y[24], tid_z[24];
    char ntid_x[24], ntid_y[24], ntid_z[24], scratch[24];
    reg_name(PC_B32, new_reg(fn, PC_B32), tid_x);
    reg_name(PC_B32, new_reg(fn, PC_B32), tid_y);
    reg_name(PC_B32, new_reg(fn, PC_B32), tid_z);
    reg_name(PC_B32, new_reg(fn, PC_B32), ntid_x);
    reg_name(PC_B32, new_reg(fn, PC_B32), ntid_y);
    reg_name(PC_B32, new_reg(fn, PC_B32), ntid_z);
    reg_name(PC_B32, new_reg(fn, PC_B32), scratch);
    sb_printf(&fn->body,
              "\tmov.u32 %s, %%tid.x;\n"
              "\tmov.u32 %s, %%tid.y;\n"
              "\tmov.u32 %s, %%tid.z;\n"
              "\tmov.u32 %s, %%ntid.x;\n"
              "\tmov.u32 %s, %%ntid.y;\n"
              "\tmov.u32 %s, %%ntid.z;\n"
              "\tmad.lo.u32 %s, %s, %s, %s;\n"
              "\tmad.lo.u32 %s, %s, %s, %s;\n"
              "\tmul.lo.u32 %s, %s, %s;\n"
              "\tmul.lo.u32 %s, %s, %s;\n"
              "\tbar.sync 0;\n",
              tid_x, tid_y, tid_z, ntid_x, ntid_y, ntid_z, scratch, tid_z,
              ntid_y, tid_y, linear, scratch, ntid_x, tid_x, participants,
              ntid_x, ntid_y, participants, participants, ntid_z);
  }

  size_t label_id = fn->call_count++;
  unsigned element_bytes = desc->element == MTLC_TENSOR_ELEMENT_FLOAT64
                               ? 8u
                           : (desc->element == MTLC_TENSOR_ELEMENT_FLOAT16 ||
                              desc->element == MTLC_TENSOR_ELEMENT_BFLOAT16)
                               ? 2u
                               : 4u;
  unsigned long tile_elements =
      (unsigned long)desc->m * (unsigned long)desc->n;
  char done[24], destination_address[24], value[24];
  reg_name(PC_PRED, new_reg(fn, PC_PRED), done);
  reg_name(compute_class, new_reg(fn, compute_class), value);
  sb_printf(
      &fn->body,
      "\t// mtlc.tensor_epilogue cooperative-memory m=%u n=%u element=%d bias=%d activation=%d scope=%s\n"
      "mtlc_tensor_epilogue_%llu_loop:\n"
      "\tsetp.ge.u32 %s, %s, %lu;\n"
      "\t@%s bra mtlc_tensor_epilogue_%llu_finish;\n"
      "\tdiv.u32 %s, %s, %u;\n"
      "\trem.u32 %s, %s, %u;\n",
      (unsigned)desc->m, (unsigned)desc->n, (int)desc->element,
      (int)desc->bias_mode, (int)desc->activation,
      desc->scope == MTLC_MEMORY_SCOPE_WORKGROUP ? "workgroup" : "subgroup",
      (unsigned long long)label_id, done, linear, tile_elements, done,
      (unsigned long long)label_id, row, linear, (unsigned)desc->n, column,
      linear, (unsigned)desc->n);
  ptx_tensor_epilogue_address(fn, destination, row, column,
                              leading_dimension, desc->layout, element_bytes,
                              destination_address);
  ptx_tensor_epilogue_load(fn, desc->element, destination_space,
                           destination_address, value);
  if (desc->scale_output)
    sb_printf(&fn->body, "\tmul.rn.%s %s, %s, %s;\n", compute_type, value,
              value, alpha);

  if (desc->bias_mode != MTLC_TENSOR_BIAS_NONE) {
    char bias_address[24], bias_value[24];
    reg_name(compute_class, new_reg(fn, compute_class), bias_value);
    if (desc->bias_mode == MTLC_TENSOR_BIAS_MATRIX) {
      ptx_tensor_epilogue_address(
          fn, bias, row, column, bias_leading_dimension, desc->bias_layout,
          element_bytes, bias_address);
    } else {
      char bias_offset[24];
      const char *bias_index =
          desc->bias_mode == MTLC_TENSOR_BIAS_PER_ROW ? row : column;
      reg_name(PC_B64, new_reg(fn, PC_B64), bias_offset);
      sb_printf(&fn->body, "\tmul.wide.u32 %s, %s, %u;\n", bias_offset,
                bias_index, element_bytes);
      reg_name(PC_B64, new_reg(fn, PC_B64), bias_address);
      sb_printf(&fn->body, "\tadd.u64 %s, %s, %s;\n", bias_address, bias,
                bias_offset);
    }
    ptx_tensor_epilogue_load(fn, desc->element, bias_space, bias_address,
                             bias_value);
    if (desc->scale_bias)
      sb_printf(&fn->body, "\tmul.rn.%s %s, %s, %s;\n", compute_type,
                bias_value, bias_value, beta);
    sb_printf(&fn->body, "\tadd.rn.%s %s, %s, %s;\n", compute_type, value,
              value, bias_value);
  }

  if (desc->activation == MTLC_TENSOR_ACTIVATION_RELU) {
    char negative[24];
    const char *positive_zero =
        compute_class == PC_F64 ? "0d0000000000000000" : "0f00000000";
    reg_name(PC_PRED, new_reg(fn, PC_PRED), negative);
    sb_printf(&fn->body,
              "\tsetp.lt.%s %s, %s, %s;\n"
              "\tselp.%s %s, %s, %s, %s;\n",
              compute_type, negative, value, positive_zero, compute_type,
              value, positive_zero, value, negative);
  } else if (desc->activation == MTLC_TENSOR_ACTIVATION_CLAMP) {
    char outside[24];
    reg_name(PC_PRED, new_reg(fn, PC_PRED), outside);
    sb_printf(&fn->body,
              "\tsetp.lt.%s %s, %s, %s;\n"
              "\tselp.%s %s, %s, %s, %s;\n"
              "\tsetp.gt.%s %s, %s, %s;\n"
              "\tselp.%s %s, %s, %s, %s;\n",
              compute_type, outside, value, clamp_minimum, compute_type,
              value, clamp_minimum, value, outside, compute_type, outside,
              value, clamp_maximum, compute_type, value, clamp_maximum, value,
              outside);
  }
  ptx_tensor_epilogue_store(fn, desc->element, destination_space,
                            destination_address, value);
  sb_printf(&fn->body,
            "\tadd.u32 %s, %s, %s;\n"
            "\tbra mtlc_tensor_epilogue_%llu_loop;\n"
            "mtlc_tensor_epilogue_%llu_finish:\n",
            linear, linear, participants, (unsigned long long)label_id,
            (unsigned long long)label_id);
  if (desc->scope == MTLC_MEMORY_SCOPE_SUBGROUP)
    sb_printf(&fn->body, "\tbar.warp.sync %s;\n", mask);
  else
    sb_puts(&fn->body, "\tbar.sync 0;\n");
}

typedef struct {
  const IRInstruction *instruction;
  const MtlcTensorEpilogueDesc *desc;
  const char *bias_space;
  PtxClass compute_class;
  const char *compute_type;
  char bias[24];
  char bias_value[24];
  char alpha[24];
  char beta[24];
  char clamp_minimum[24];
  char clamp_maximum[24];
  char bias_leading_dimension[24];
  char predicate[24];
} PtxResidentEpilogue;

static size_t ptx_tensor_mma_stride_operand_index(
    const MtlcTensorMmaDesc *desc, unsigned requested) {
  if (!desc) return SIZE_MAX;
  size_t index =
      4u + (desc->sparsity != MTLC_TENSOR_SPARSITY_DENSE ? 1u : 0u) +
      (desc->a_scale_mode != MTLC_TENSOR_SCALE_NONE ? 1u : 0u) +
      (desc->b_scale_mode != MTLC_TENSOR_SCALE_NONE ? 1u : 0u);
  unsigned mask = ir_tensor_mma_runtime_stride_mask(desc);
  for (unsigned bit = MTLC_TENSOR_RUNTIME_STRIDE_A;
       bit <= MTLC_TENSOR_RUNTIME_STRIDE_D; bit <<= 1) {
    if (!(mask & bit)) continue;
    if (bit == requested) return index;
    index++;
  }
  return SIZE_MAX;
}

static size_t ptx_tensor_epilogue_stride_operand_index(
    const MtlcTensorEpilogueDesc *desc) {
  if (!desc || desc->leading_dimension != 0) return SIZE_MAX;
  size_t index = 1u;
  if (desc->bias_mode != MTLC_TENSOR_BIAS_NONE) index++;
  if (desc->scale_output) index++;
  if (desc->scale_bias) index++;
  if (desc->activation == MTLC_TENSOR_ACTIVATION_CLAMP) index += 2u;
  return index;
}

static int ptx_tensor_epilogue_handoff_compatible(
    const IRInstruction *mma, const IRInstruction *epilogue) {
  if (!mma || !epilogue ||
      (mma->op != IR_OP_TENSOR_MMA && mma->op != IR_OP_TENSOR_COMMIT) ||
      epilogue->op != IR_OP_TENSOR_EPILOGUE ||
      !ir_tensor_mma_desc_valid(&IR_TENSOR_MMA(mma)) ||
      !ir_tensor_epilogue_desc_valid(&IR_TENSOR_EPILOGUE(epilogue)))
    return 0;
  const MtlcTensorMmaDesc *md = &IR_TENSOR_MMA(mma);
  const MtlcTensorEpilogueDesc *ed = &IR_TENSOR_EPILOGUE(epilogue);
  if (md->m != ed->m || md->n != ed->n ||
      md->result_element != ed->element || md->d_layout != ed->layout ||
      md->scope != ed->scope)
    return 0;
  size_t per_tile = ir_tensor_mma_operand_count(md);
  size_t tile_count = mma->op == IR_OP_TENSOR_MMA
                          ? ir_tensor_mma_instruction_count(mma)
                          : 1u;
  size_t epilogue_count = ir_tensor_epilogue_operand_count(ed);
  if (!per_tile || !tile_count ||
      tile_count > SIZE_MAX / per_tile ||
      mma->argument_count != tile_count * per_tile ||
      !epilogue_count || epilogue->argument_count != epilogue_count)
    return 0;
  size_t base = (tile_count - 1u) * per_tile;
  if (!ir_operand_same(&mma->arguments[base + 3u],
                       &epilogue->arguments[0]))
    return 0;
  if (md->d_leading_dimension != ed->leading_dimension) return 0;
  if (md->d_leading_dimension == 0) {
    size_t mma_stride =
        ptx_tensor_mma_stride_operand_index(md,
                                            MTLC_TENSOR_RUNTIME_STRIDE_D);
    size_t epilogue_stride =
        ptx_tensor_epilogue_stride_operand_index(ed);
    if (mma_stride == SIZE_MAX || epilogue_stride == SIZE_MAX ||
        !ir_operand_same(&mma->arguments[base + mma_stride],
                         &epilogue->arguments[epilogue_stride]))
      return 0;
  }
  return 1;
}

static const IRInstruction *ptx_following_tensor_epilogue(
    const IRFunction *function, size_t instruction_index,
    size_t *epilogue_index) {
  if (!function || instruction_index >= function->instruction_count)
    return NULL;
  for (size_t i = instruction_index + 1u;
       i < function->instruction_count; i++) {
    const IRInstruction *candidate = &function->instructions[i];
    if (candidate->op == IR_OP_NOP) continue;
    if (candidate->op != IR_OP_TENSOR_EPILOGUE) return NULL;
    if (epilogue_index) *epilogue_index = i;
    return candidate;
  }
  return NULL;
}

static const IRInstruction *ptx_loop_exit_tensor_epilogue(
    const IRFunction *function, size_t commit_index,
    size_t *epilogue_index) {
  if (!function || commit_index >= function->instruction_count) return NULL;
  const IRInstruction *commit = &function->instructions[commit_index];
  if (commit->op != IR_OP_TENSOR_COMMIT ||
      commit->tensor_residency_scope != IR_TENSOR_RESIDENCY_SCOPE_LOOP)
    return NULL;

  size_t jump_index = SIZE_MAX;
  for (size_t i = commit_index + 1u; i < function->instruction_count; i++) {
    const IRInstruction *candidate = &function->instructions[i];
    if (candidate->op == IR_OP_NOP) continue;
    if (candidate->op != IR_OP_JUMP || !candidate->text) return NULL;
    jump_index = i;
    break;
  }
  if (jump_index == SIZE_MAX) return NULL;
  const char *exit_label = function->instructions[jump_index].text;

  size_t label_index = SIZE_MAX;
  for (size_t i = jump_index + 1u; i < function->instruction_count; i++) {
    const IRInstruction *candidate = &function->instructions[i];
    if (candidate->op == IR_OP_NOP) continue;
    if (candidate->op != IR_OP_LABEL || !candidate->text ||
        strcmp(candidate->text, exit_label) != 0)
      return NULL;
    label_index = i;
    break;
  }
  if (label_index == SIZE_MAX) return NULL;

  size_t predecessor_count = 0;
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *candidate = &function->instructions[i];
    if ((candidate->op == IR_OP_JUMP ||
         candidate->op == IR_OP_BRANCH_ZERO ||
         candidate->op == IR_OP_BRANCH_EQ) &&
        candidate->text && strcmp(candidate->text, exit_label) == 0) {
      predecessor_count++;
      if (i != jump_index) return NULL;
    }
  }
  if (predecessor_count != 1u) return NULL;

  for (size_t i = label_index + 1u; i < function->instruction_count; i++) {
    const IRInstruction *candidate = &function->instructions[i];
    if (candidate->op == IR_OP_NOP) continue;
    if (candidate->op != IR_OP_TENSOR_EPILOGUE) return NULL;
    if (epilogue_index) *epilogue_index = i;
    return candidate;
  }
  return NULL;
}

static int ptx_tensor_epilogue_was_consumed(
    const PtxFn *fn, const IRInstruction *epilogue) {
  if (!fn || !epilogue) return 0;
  for (size_t i = 0; i < fn->tensor_residency_count; i++) {
    if (fn->tensor_residencies[i].consumed_epilogue == epilogue) return 1;
  }
  return 0;
}

static int ptx_prepare_resident_epilogue(PtxFn *fn,
                                         const IRInstruction *epilogue,
                                         PtxResidentEpilogue *state) {
  if (!fn || !epilogue || !state ||
      epilogue->op != IR_OP_TENSOR_EPILOGUE ||
      !ir_tensor_epilogue_desc_valid(&IR_TENSOR_EPILOGUE(epilogue)))
    return 0;
  memset(state, 0, sizeof(*state));
  state->instruction = epilogue;
  state->desc = &IR_TENSOR_EPILOGUE(epilogue);
  state->compute_class =
      state->desc->element == MTLC_TENSOR_ELEMENT_FLOAT64 ? PC_F64 : PC_F32;
  state->compute_type = state->compute_class == PC_F64 ? "f64" : "f32";
  size_t argument = 1u;
  if (state->desc->bias_mode != MTLC_TENSOR_BIAS_NONE) {
    PtxVal bias_desc = operand_desc(fn, &epilogue->arguments[argument]);
    state->bias_space = ptx_wmma_space(bias_desc);
    if (!bias_desc.is_ptr || !state->bias_space) return 0;
    use_as(fn, &epilogue->arguments[argument++], PC_B64, state->bias);
    reg_name(state->compute_class, new_reg(fn, state->compute_class),
             state->bias_value);
  }
  if (state->desc->scale_output)
    use_as(fn, &epilogue->arguments[argument++], state->compute_class,
           state->alpha);
  if (state->desc->scale_bias)
    use_as(fn, &epilogue->arguments[argument++], state->compute_class,
           state->beta);
  if (state->desc->activation == MTLC_TENSOR_ACTIVATION_CLAMP) {
    use_as(fn, &epilogue->arguments[argument++], state->compute_class,
           state->clamp_minimum);
    use_as(fn, &epilogue->arguments[argument++], state->compute_class,
           state->clamp_maximum);
  }
  if (state->desc->leading_dimension == 0) argument++;
  if (state->desc->bias_mode == MTLC_TENSOR_BIAS_MATRIX) {
    if (state->desc->bias_leading_dimension == 0) {
      use_as(fn, &epilogue->arguments[argument++], PC_B32,
             state->bias_leading_dimension);
    } else {
      reg_name(PC_B32, new_reg(fn, PC_B32),
               state->bias_leading_dimension);
      sb_printf(&fn->body, "\tmov.u32 %s, %u;\n",
                state->bias_leading_dimension,
                state->desc->bias_leading_dimension);
    }
  }
  if (argument != epilogue->argument_count) return 0;
  if (state->desc->activation != MTLC_TENSOR_ACTIVATION_IDENTITY)
    reg_name(PC_PRED, new_reg(fn, PC_PRED), state->predicate);
  return 1;
}

static void ptx_emit_resident_epilogue_barrier(PtxFn *fn, char mask[24]) {
  reg_name(PC_B32, new_reg(fn, PC_B32), mask);
  sb_printf(&fn->body,
            "\tactivemask.b32 %s;\n"
            "\tbar.warp.sync %s;\n",
            mask, mask);
}

static void ptx_apply_resident_epilogue_value(
    PtxFn *fn, const PtxResidentEpilogue *state, const char *value,
    int have_bias) {
  if (state->desc->scale_output)
    sb_printf(&fn->body, "\tmul.rn.%s %s, %s, %s;\n",
              state->compute_type, value, value, state->alpha);
  if (have_bias) {
    if (state->desc->scale_bias)
      sb_printf(&fn->body, "\tmul.rn.%s %s, %s, %s;\n",
                state->compute_type, state->bias_value, state->bias_value,
                state->beta);
    sb_printf(&fn->body, "\tadd.rn.%s %s, %s, %s;\n",
              state->compute_type, value, value, state->bias_value);
  }
  if (state->desc->activation == MTLC_TENSOR_ACTIVATION_RELU) {
    const char *positive_zero =
        state->compute_class == PC_F64 ? "0d0000000000000000"
                                       : "0f00000000";
    sb_printf(&fn->body,
              "\tsetp.lt.%s %s, %s, %s;\n"
              "\tselp.%s %s, %s, %s, %s;\n",
              state->compute_type, state->predicate, value, positive_zero,
              state->compute_type, value, positive_zero, value,
              state->predicate);
  } else if (state->desc->activation ==
             MTLC_TENSOR_ACTIVATION_CLAMP) {
    sb_printf(&fn->body,
              "\tsetp.lt.%s %s, %s, %s;\n"
              "\tselp.%s %s, %s, %s, %s;\n"
              "\tsetp.gt.%s %s, %s, %s;\n"
              "\tselp.%s %s, %s, %s, %s;\n",
              state->compute_type, state->predicate, value,
              state->clamp_minimum, state->compute_type, value,
              state->clamp_minimum, value, state->predicate,
              state->compute_type, state->predicate, value,
              state->clamp_maximum, state->compute_type, value,
              state->clamp_maximum, value, state->predicate);
  }
}

static int ptx_resident_epilogue_tuple_cost(
    const MtlcTensorEpilogueDesc *desc) {
  if (!desc) return INT_MAX;
  int cost = desc->activation == MTLC_TENSOR_ACTIVATION_IDENTITY ? 0 : 1;
  cost += desc->scale_output ? 1 : 0;
  cost += desc->activation == MTLC_TENSOR_ACTIVATION_CLAMP ? 2 : 0;
  if (desc->bias_mode != MTLC_TENSOR_BIAS_NONE)
    cost += 7 + (desc->scale_bias ? 1 : 0);
  return cost;
}

static int ptx_stable_resident_epilogue_capable(
    const PtxWmmaProfile *profile, const MtlcTensorEpilogueDesc *desc) {
  if (!profile || !desc || desc->bias_mode != MTLC_TENSOR_BIAS_NONE)
    return 0;
  return (desc->element == MTLC_TENSOR_ELEMENT_FLOAT32 &&
          profile->d_class == PC_F32) ||
         (desc->element == MTLC_TENSOR_ELEMENT_FLOAT64 &&
          profile->d_class == PC_F64);
}

static int ptx_native_resident_epilogue_capable(
    const PtxMmaProfile *profile, const MtlcTensorEpilogueDesc *desc) {
  return profile && desc && profile->accumulator_registers > 0 &&
         desc->element == MTLC_TENSOR_ELEMENT_FLOAT32;
}

static int ptx_apply_stable_resident_epilogue(
    PtxFn *fn, const IRInstruction *epilogue,
    const PtxWmmaProfile *profile, int accumulator_base,
    int accumulator_count, char mask[24]) {
  if (!ptx_stable_resident_epilogue_capable(
          profile, &IR_TENSOR_EPILOGUE(epilogue)))
    return 0;
  PtxResidentEpilogue state;
  if (!ptx_prepare_resident_epilogue(fn, epilogue, &state)) return 0;
  ptx_emit_resident_epilogue_barrier(fn, mask);
  for (int i = 0; i < accumulator_count; i++) {
    char value[24];
    reg_name(profile->d_class, accumulator_base + i, value);
    ptx_apply_resident_epilogue_value(fn, &state, value, 0);
  }
  return !fn->error;
}

static void ptx_native_epilogue_bias_address(
    PtxFn *fn, const PtxResidentEpilogue *state, PtxMmaCoordinate row,
    PtxMmaCoordinate column, const char *group, const char *thread,
    char address[24]) {
  if (state->desc->bias_mode == MTLC_TENSOR_BIAS_MATRIX) {
    ptx_mma_emit_address(fn, state->bias, state->bias_leading_dimension,
                         state->desc->bias_layout, 0, row, column, 4,
                         group, thread, address);
    return;
  }
  char index[24], byte_offset[24];
  ptx_mma_emit_coordinate(
      fn, state->desc->bias_mode == MTLC_TENSOR_BIAS_PER_ROW ? row : column,
      group, thread, index);
  reg_name(PC_B64, new_reg(fn, PC_B64), byte_offset);
  sb_printf(&fn->body, "\tmul.wide.u32 %s, %s, 4;\n", byte_offset,
            index);
  reg_name(PC_B64, new_reg(fn, PC_B64), address);
  sb_printf(&fn->body, "\tadd.u64 %s, %s, %s;\n", address,
            state->bias, byte_offset);
}

static void ptx_apply_store_native_resident_epilogue_subtile(
    PtxFn *fn, const IRInstruction *mma, const PtxMmaProfile *profile,
    const PtxResidentEpilogue *state, const char *dp, const char *dspace,
    const char *d_stride, const char *group, const char *thread,
    unsigned m_offset, unsigned n_offset, int accumulator_base) {
  for (int element = 0; element < profile->accumulator_registers; element++) {
    PtxMmaCoordinate row = {
        1, 0, m_offset + (element >= 2 ? 8u : 0u)};
    PtxMmaCoordinate column = {
        0, 2, n_offset + (unsigned)(element & 1)};
    char value[24];
    reg_name(PC_F32, accumulator_base + element, value);
    int have_bias = state->desc->bias_mode != MTLC_TENSOR_BIAS_NONE;
    if (have_bias) {
      char bias_address[24];
      ptx_native_epilogue_bias_address(fn, state, row, column, group, thread,
                                       bias_address);
      sb_printf(&fn->body, "\tld%s.f32 %s, [%s];\n", state->bias_space,
                state->bias_value, bias_address);
    }
    ptx_apply_resident_epilogue_value(fn, state, value, have_bias);
    ptx_mma_store_f32(fn, dp, d_stride, IR_TENSOR_MMA(mma).d_layout, dspace,
                      row, column, group, thread,
                      accumulator_base + element);
  }
}

static int ptx_try_emit_tensor_mma_resident_epilogue(
    PtxFn *fn, const IRInstruction *mma, const IRInstruction *epilogue) {
  if (!fn || !mma || !epilogue || mma->op != IR_OP_TENSOR_MMA ||
      mma->tensor_residency_role != IR_TENSOR_RESIDENCY_NONE ||
      mma->tensor_residency_id != 0 ||
      !ptx_tensor_epilogue_handoff_compatible(mma, epilogue))
    return 0;
  size_t tile_count = ir_tensor_mma_instruction_count(mma);
  size_t per_tile = ir_tensor_mma_operand_count(&IR_TENSOR_MMA(mma));
  int tuple_budget = ptx_tensor_tuple_budget(fn);
  int epilogue_cost =
      ptx_resident_epilogue_tuple_cost(&IR_TENSOR_EPILOGUE(epilogue));

  if (ptx_tensor_uses_direct_mma(&IR_TENSOR_MMA(mma))) {
    PtxMmaProfile profile;
    char reason[256];
    if (!ptx_select_mma_profile(fn, &IR_TENSOR_MMA(mma), &profile, reason,
                                sizeof(reason)) ||
        !ptx_native_resident_epilogue_capable(
            &profile, &IR_TENSOR_EPILOGUE(epilogue)))
      return 0;
    int subtile_count = profile.m_tiles * profile.n_tiles;
    int accumulator_count =
        subtile_count * profile.accumulator_registers;
    int estimated_peak = accumulator_count + profile.a_registers +
                         profile.b_registers + epilogue_cost;
    if (estimated_peak > tuple_budget) return 0;
    int accumulator_base = fn->count[PC_F32];
    for (int i = 0; i < accumulator_count; i++) new_reg(fn, PC_F32);
    char lane[24], group[24], thread[24];
    reg_name(PC_B32, new_reg(fn, PC_B32), lane);
    reg_name(PC_B32, new_reg(fn, PC_B32), group);
    reg_name(PC_B32, new_reg(fn, PC_B32), thread);
    sb_printf(
        &fn->body,
        "\t// mtlc.tensor_epilogue resident native-mma %s tiles=%llu subtiles=%d tuple_peak=%d budget=%d\n"
        "\tmov.u32 %s, %%laneid;\n"
        "\tshr.u32 %s, %s, 2;\n"
        "\tand.b32 %s, %s, 3;\n",
        ptx_mma_kind_name(&profile), (unsigned long long)tile_count,
        subtile_count, estimated_peak, tuple_budget, lane, group, lane,
        thread, lane);
    for (size_t tile = 0; tile < tile_count; tile++) {
      size_t base = tile * per_tile;
      PtxMmaTileMemory memory;
      if (!ptx_mma_prepare_tile_memory(fn, mma, &profile, base, per_tile,
                                       &memory)) {
        fn_error(fn,
                 "PTX native resident epilogue has invalid tile memory");
        return 1;
      }
      for (int m = 0; m < profile.m_tiles; m++) {
        PtxMmaSparseAFragment sparse_a;
        PtxMmaSparseAFragment *sparse_a_ptr = NULL;
        if (ptx_mma_profile_is_sparse(&profile)) {
          ptx_mma_prepare_sparse_f16_a(
              fn, mma, &memory, group, thread, (unsigned)m * 16u, &sparse_a);
          sparse_a_ptr = &sparse_a;
        }
        for (int n = 0; n < profile.n_tiles; n++) {
          int subtile = m * profile.n_tiles + n;
          ptx_emit_mma_native_subtile(
              fn, mma, &profile, &memory, group, thread, sparse_a_ptr,
              (unsigned)m * 16u, (unsigned)n * 8u,
              accumulator_base +
                  subtile * profile.accumulator_registers,
              tile == 0, 0);
        }
      }
    }
    size_t final_base = (tile_count - 1u) * per_tile;
    PtxVal dv = operand_desc(fn, &mma->arguments[final_base + 3u]);
    const char *dspace = ptx_wmma_space(dv);
    char dp[24], strides[4][24], mask[24];
    if (!dv.is_ptr || !dspace) {
      fn_error(fn, "PTX resident tensor epilogue requires a writable D tile");
      return 1;
    }
    use_as(fn, &mma->arguments[final_base + 3u], PC_B64, dp);
    if (!ptx_tensor_stride_registers(fn, mma, final_base, per_tile,
                                     strides)) {
      fn_error(fn, "PTX resident tensor epilogue has inconsistent D stride");
      return 1;
    }
    PtxResidentEpilogue state;
    if (!ptx_prepare_resident_epilogue(fn, epilogue, &state)) {
      fn_error(fn, "PTX resident tensor epilogue has invalid operands");
      return 1;
    }
    ptx_emit_resident_epilogue_barrier(fn, mask);
    for (int m = 0; m < profile.m_tiles; m++) {
      for (int n = 0; n < profile.n_tiles; n++) {
        int subtile = m * profile.n_tiles + n;
        ptx_apply_store_native_resident_epilogue_subtile(
            fn, mma, &profile, &state, dp, dspace, strides[3], group, thread,
            (unsigned)m * 16u, (unsigned)n * 8u,
            accumulator_base +
                subtile * profile.accumulator_registers);
      }
    }
    sb_printf(&fn->body, "\tbar.warp.sync %s;\n", mask);
    return 1;
  }

  PtxWmmaProfile profile;
  char reason[256];
  if (!ptx_select_wmma_profile(fn, &IR_TENSOR_MMA(mma), &profile, reason,
                               sizeof(reason)) ||
      !ptx_stable_resident_epilogue_capable(
          &profile, &IR_TENSOR_EPILOGUE(epilogue)))
    return 0;
  int subtile_count = profile.m_tiles * profile.n_tiles;
  int accumulator_count = subtile_count * profile.d_registers;
  int estimated_peak = accumulator_count + profile.a_registers +
                       profile.b_registers + profile.c_registers +
                       epilogue_cost;
  if (estimated_peak > tuple_budget) return 0;
  int accumulator_base = fn->count[profile.d_class];
  for (int i = 0; i < accumulator_count; i++)
    new_reg(fn, profile.d_class);
  sb_printf(&fn->body,
            "\t// mtlc.tensor_epilogue resident stable-wmma tiles=%llu subtiles=%d tuple_peak=%d budget=%d\n",
            (unsigned long long)tile_count, subtile_count, estimated_peak,
            tuple_budget);
  for (size_t tile = 0; tile < tile_count; tile++) {
    if (!ptx_emit_wmma_tiled_tile(
            fn, mma, &profile, tile * per_tile, per_tile, accumulator_base,
            tile == 0, 0))
      return 1;
  }
  char mask[24];
  if (!ptx_apply_stable_resident_epilogue(
          fn, epilogue, &profile, accumulator_base, accumulator_count, mask)) {
    fn_error(fn, "PTX could not apply a stable resident tensor epilogue");
    return 1;
  }
  size_t final_base = (tile_count - 1u) * per_tile;
  if (!ptx_emit_wmma_tiled_store(fn, mma, &profile, final_base, per_tile,
                                  accumulator_base))
    return 1;
  sb_printf(&fn->body, "\tbar.warp.sync %s;\n", mask);
  return 1;
}

static int ptx_try_emit_tensor_commit_resident_epilogue(
    PtxFn *fn, const IRInstruction *commit,
    const IRInstruction *epilogue) {
  if (!fn || !commit || !epilogue ||
      commit->op != IR_OP_TENSOR_COMMIT ||
      !ptx_tensor_epilogue_handoff_compatible(commit, epilogue))
    return 0;
  PtxTensorResidency *group =
      ptx_tensor_residency_find(fn, commit->tensor_residency_id);
  if (!group || !group->resident) return 0;
  size_t per_tile = ir_tensor_mma_operand_count(&IR_TENSOR_MMA(commit));
  int total_peak =
      group->tuple_peak +
      ptx_resident_epilogue_tuple_cost(&IR_TENSOR_EPILOGUE(epilogue));
  if (total_peak > ptx_tensor_tuple_budget(fn)) return 0;

  if (ptx_tensor_uses_direct_mma(&IR_TENSOR_MMA(commit))) {
    PtxMmaProfile profile;
    char reason[256];
    if (!ptx_select_mma_profile(fn, &IR_TENSOR_MMA(commit), &profile, reason,
                                sizeof(reason)) ||
        !ptx_native_resident_epilogue_capable(
            &profile, &IR_TENSOR_EPILOGUE(epilogue)))
      return 0;
    PtxVal dv = operand_desc(fn, &commit->arguments[3]);
    const char *dspace = ptx_wmma_space(dv);
    char dp[24], strides[4][24], lane[24], lane_group[24], thread[24];
    char mask[24];
    if (!dv.is_ptr || !dspace) return 0;
    use_as(fn, &commit->arguments[3], PC_B64, dp);
    if (!ptx_tensor_stride_registers(fn, commit, 0, per_tile, strides))
      return 0;
    reg_name(PC_B32, new_reg(fn, PC_B32), lane);
    reg_name(PC_B32, new_reg(fn, PC_B32), lane_group);
    reg_name(PC_B32, new_reg(fn, PC_B32), thread);
    sb_printf(&fn->body,
              "\t// mtlc.tensor_epilogue resident handoff group=%u native-mma %s tuple_peak=%d budget=%d\n"
              "\tmov.u32 %s, %%laneid;\n"
              "\tshr.u32 %s, %s, 2;\n"
              "\tand.b32 %s, %s, 3;\n",
              group->id, ptx_mma_kind_name(&profile), total_peak,
              ptx_tensor_tuple_budget(fn), lane, lane_group, lane, thread,
              lane);
    PtxResidentEpilogue state;
    if (!ptx_prepare_resident_epilogue(fn, epilogue, &state)) {
      fn_error(fn, "PTX resident tensor epilogue handoff has invalid operands");
      return 1;
    }
    ptx_emit_resident_epilogue_barrier(fn, mask);
    for (int m = 0; m < profile.m_tiles; m++) {
      for (int n = 0; n < profile.n_tiles; n++) {
        int subtile = m * profile.n_tiles + n;
        ptx_apply_store_native_resident_epilogue_subtile(
            fn, commit, &profile, &state, dp, dspace, strides[3],
            lane_group, thread, (unsigned)m * 16u, (unsigned)n * 8u,
            group->accumulator_base +
                subtile * profile.accumulator_registers);
      }
    }
    sb_printf(&fn->body, "\tbar.warp.sync %s;\n", mask);
    return 1;
  }

  PtxWmmaProfile profile;
  char reason[256];
  if (!ptx_select_wmma_profile(fn, &IR_TENSOR_MMA(commit), &profile, reason,
                               sizeof(reason)) ||
      !ptx_stable_resident_epilogue_capable(
          &profile, &IR_TENSOR_EPILOGUE(epilogue)))
    return 0;
  int accumulator_count =
      profile.m_tiles * profile.n_tiles * profile.d_registers;
  if (group->accumulator_class != profile.d_class ||
      group->accumulator_count != accumulator_count)
    return 0;
  sb_printf(&fn->body,
            "\t// mtlc.tensor_epilogue resident handoff group=%u stable-wmma tuple_peak=%d budget=%d\n",
            group->id, total_peak, ptx_tensor_tuple_budget(fn));
  char mask[24];
  if (!ptx_apply_stable_resident_epilogue(
          fn, epilogue, &profile, group->accumulator_base,
          accumulator_count, mask)) {
    fn_error(fn, "PTX could not apply a stable resident epilogue handoff");
    return 1;
  }
  if (!ptx_emit_wmma_tiled_store(fn, commit, &profile, 0, per_tile,
                                  group->accumulator_base))
    return 1;
  sb_printf(&fn->body, "\tbar.warp.sync %s;\n", mask);
  return 1;
}

static void ptx_emit_tensor_mma(PtxFn *fn, const IRInstruction *in) {
  if (in->tensor_residency_role == IR_TENSOR_RESIDENCY_START ||
      in->tensor_residency_role == IR_TENSOR_RESIDENCY_UPDATE) {
    ptx_emit_tensor_residency_mma(fn, in);
    return;
  }
  if (in->tensor_residency_role != IR_TENSOR_RESIDENCY_NONE ||
      in->tensor_residency_id != 0 ||
      in->tensor_residency_scope != IR_TENSOR_RESIDENCY_SCOPE_NONE) {
    fn_error(fn, "PTX tensor MMA has inconsistent residency metadata");
    return;
  }
  size_t tile_count = ir_tensor_mma_instruction_count(in);
  if (tile_count <= 1) {
    ptx_emit_tensor_mma_single(fn, in);
    return;
  }
  size_t per_tile = ir_tensor_mma_operand_count(&IR_TENSOR_MMA(in));
  if (!per_tile || tile_count > SIZE_MAX / per_tile ||
      in->argument_count != per_tile * tile_count) {
    fn_error(fn, "PTX tensor chain operand count is inconsistent");
    return;
  }
  int tuple_budget = ptx_tensor_tuple_budget(fn);
  if (ptx_tensor_uses_direct_mma(&IR_TENSOR_MMA(in))) {
    PtxMmaProfile native_profile;
    char native_reason[256];
    if (!ptx_select_mma_profile(fn, &IR_TENSOR_MMA(in), &native_profile,
                                native_reason, sizeof(native_reason))) {
      fn_error(fn, "PTX native tensor MMA chain cannot lower m%un%uk%u: %s",
               (unsigned)IR_TENSOR_MMA(in).m,
               (unsigned)IR_TENSOR_MMA(in).n,
               (unsigned)IR_TENSOR_MMA(in).k, native_reason);
      return;
    }
    int accumulator_count = native_profile.m_tiles * native_profile.n_tiles *
                            native_profile.accumulator_registers;
    int estimated_peak = accumulator_count + native_profile.a_registers +
                         native_profile.b_registers;
    if (estimated_peak <= tuple_budget) {
      ptx_report_tensor_instruction(
          fn, in, "mma.sync", native_profile.shape,
          ptx_mma_kind_name(&native_profile),
          native_profile.m_tiles * native_profile.n_tiles);
      ptx_report_tensor_resident(fn, in, "the chain", estimated_peak,
                                 tuple_budget);
      ptx_emit_tensor_mma_native_chain_resident(
          fn, in, &native_profile, tile_count, per_tile, tuple_budget,
          estimated_peak);
      return;
    }
    ptx_report_tensor_replay(fn, in, "the chain", estimated_peak,
                             tuple_budget);
    sb_printf(
        &fn->body,
        "\t// mtlc.tensor_chain replay native-mma %s tiles=%llu tuple_peak=%d budget=%d\n",
        ptx_mma_kind_name(&native_profile),
        (unsigned long long)tile_count, estimated_peak, tuple_budget);
    for (size_t tile = 0; tile < tile_count; tile++) {
      IRInstruction single = *in;
      single.arguments = &in->arguments[tile * per_tile];
      single.argument_types =
          in->argument_types ? &in->argument_types[tile * per_tile] : NULL;
      single.argument_count = per_tile;
      single.tensor_mma_count = 1;
      ptx_emit_tensor_mma_single(fn, &single);
      if (fn->error) return;
    }
    return;
  }
  PtxWmmaProfile profile;
  char reason[256];
  if (!ptx_select_wmma_profile(fn, &IR_TENSOR_MMA(in), &profile, reason,
                               sizeof(reason))) {
    fn_error(fn, "PTX tensor MMA chain cannot lower m%un%uk%u: %s",
             (unsigned)IR_TENSOR_MMA(in).m, (unsigned)IR_TENSOR_MMA(in).n,
             (unsigned)IR_TENSOR_MMA(in).k, reason);
    return;
  }
  int subtile_count = profile.m_tiles * profile.n_tiles;
  int estimated_peak = subtile_count * profile.d_registers +
                       profile.a_registers + profile.b_registers +
                       profile.c_registers;
  if (estimated_peak <= tuple_budget) {
    ptx_report_tensor_instruction(fn, in, "wmma.mma", profile.shape,
                                  profile.a_type, subtile_count);
    ptx_report_tensor_resident(fn, in, "the chain", estimated_peak,
                               tuple_budget);
    ptx_emit_tensor_mma_chain_resident(fn, in, &profile, tile_count, per_tile,
                                       tuple_budget, estimated_peak);
    return;
  }
  ptx_report_tensor_replay(fn, in, "the chain", estimated_peak, tuple_budget);
  if (subtile_count == 1)
    sb_printf(&fn->body,
              "\t// mtlc.tensor_chain replay tiles=%llu tuple_peak=%d budget=%d\n",
              (unsigned long long)tile_count, estimated_peak, tuple_budget);
  else
    sb_printf(&fn->body,
              "\t// mtlc.tensor_chain replay tiles=%llu subtiles=%d tuple_peak=%d budget=%d\n",
              (unsigned long long)tile_count, subtile_count, estimated_peak,
              tuple_budget);
  for (size_t tile = 0; tile < tile_count; tile++) {
    IRInstruction single = *in;
    single.arguments = &in->arguments[tile * per_tile];
    single.argument_types = in->argument_types
                                ? &in->argument_types[tile * per_tile]
                                : NULL;
    single.argument_count = per_tile;
    single.tensor_mma_count = 1;
    ptx_emit_tensor_mma_single(fn, &single);
    if (fn->error) return;
  }
}

static int ptx_tensor_matmul_element_bytes(MtlcTensorElement element) {
  switch (element) {
  case MTLC_TENSOR_ELEMENT_FLOAT16:
  case MTLC_TENSOR_ELEMENT_BFLOAT16:
    return 2;
  case MTLC_TENSOR_ELEMENT_FLOAT32:
  case MTLC_TENSOR_ELEMENT_INT32:
    return 4;
  case MTLC_TENSOR_ELEMENT_FLOAT64:
    return 8;
  case MTLC_TENSOR_ELEMENT_INT8:
  case MTLC_TENSOR_ELEMENT_UINT8:
  case MTLC_TENSOR_ELEMENT_FLOAT8_E4M3:
  case MTLC_TENSOR_ELEMENT_FLOAT8_E5M2:
  case MTLC_TENSOR_ELEMENT_FLOAT6_E2M3:
  case MTLC_TENSOR_ELEMENT_FLOAT6_E3M2:
  case MTLC_TENSOR_ELEMENT_FLOAT4_E2M1:
    return 1;
  default:
    return 0;
  }
}

static unsigned ptx_tensor_matmul_narrow_bits(MtlcTensorElement element) {
  switch (element) {
  case MTLC_TENSOR_ELEMENT_FLOAT8_E4M3:
  case MTLC_TENSOR_ELEMENT_FLOAT8_E5M2:
    return 8;
  case MTLC_TENSOR_ELEMENT_FLOAT6_E2M3:
  case MTLC_TENSOR_ELEMENT_FLOAT6_E3M2:
    return 6;
  case MTLC_TENSOR_ELEMENT_FLOAT4_E2M1:
    return 4;
  default:
    return 0;
  }
}

static MtlcTensorLayout
ptx_tensor_matmul_transposed_layout(MtlcTensorLayout layout) {
  if (layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR)
    return MTLC_TENSOR_LAYOUT_COLUMN_MAJOR;
  if (layout == MTLC_TENSOR_LAYOUT_COLUMN_MAJOR)
    return MTLC_TENSOR_LAYOUT_ROW_MAJOR;
  return MTLC_TENSOR_LAYOUT_INVALID;
}

static int ptx_tensor_matmul_capability(PtxFn *fn,
                                        const MtlcTensorMmaDesc *desc) {
  if (!ir_tensor_mma_desc_valid(desc)) {
    fn_error(fn, "PTX tensor_matmul received an invalid descriptor");
    return 0;
  }
  if (desc->scope != MTLC_MEMORY_SCOPE_SUBGROUP) {
    fn_error(fn,
             "PTX tensor_matmul currently requires subgroup scope for exact native/cooperative ordering");
    return 0;
  }
  if (fn->target_arch < 70 || fn->isa_major < 6) {
    fn_error(fn,
             "PTX tensor_matmul subgroup ordering requires PTX 6.0 and sm_70 or newer");
    return 0;
  }
  if (desc->math_mode != MTLC_TENSOR_MATH_MULTIPLY_ADD) {
    fn_error(fn,
             "PTX tensor_matmul exact edge lowering currently supports multiply-add operands only");
    return 0;
  }
  int dense = desc->sparsity == MTLC_TENSOR_SPARSITY_DENSE;
  if (!dense &&
      desc->sparsity != MTLC_TENSOR_SPARSITY_STRUCTURED_2_TO_4) {
    fn_error(fn,
             "PTX tensor_matmul exact sparse edge lowering currently supports canonical structured 2:4 A only");
    return 0;
  }
  if (desc->rounding != MTLC_TENSOR_ROUND_DEFAULT &&
      desc->rounding != MTLC_TENSOR_ROUND_NEAREST_EVEN) {
    fn_error(fn,
             "PTX tensor_matmul exact edge lowering supports default/nearest-even rounding only");
    return 0;
  }
  if (desc->accumulator_element != desc->result_element) {
    fn_error(fn,
             "PTX tensor_matmul requires identical accumulator/result formats so native K chunks and exact tails compose without an intermediate narrowing");
    return 0;
  }

  int unscaled = desc->a_scale_mode == MTLC_TENSOR_SCALE_NONE &&
                 desc->b_scale_mode == MTLC_TENSOR_SCALE_NONE;
  int logical = desc->a_packing == MTLC_TENSOR_PACKING_LOGICAL &&
                desc->b_packing == MTLC_TENSOR_PACKING_LOGICAL;
  int f16_family = dense && unscaled && logical &&
      (desc->a_element == MTLC_TENSOR_ELEMENT_FLOAT16 ||
       desc->a_element == MTLC_TENSOR_ELEMENT_BFLOAT16) &&
      (desc->b_element == MTLC_TENSOR_ELEMENT_FLOAT16 ||
       desc->b_element == MTLC_TENSOR_ELEMENT_BFLOAT16) &&
      desc->accumulator_element == MTLC_TENSOR_ELEMENT_FLOAT32;
  int f64_family = dense && unscaled && logical &&
                    desc->a_element == MTLC_TENSOR_ELEMENT_FLOAT64 &&
                    desc->b_element == MTLC_TENSOR_ELEMENT_FLOAT64 &&
                    desc->accumulator_element == MTLC_TENSOR_ELEMENT_FLOAT64;
  int i8_family = dense && unscaled && logical &&
      (desc->a_element == MTLC_TENSOR_ELEMENT_INT8 ||
       desc->a_element == MTLC_TENSOR_ELEMENT_UINT8) &&
      (desc->b_element == MTLC_TENSOR_ELEMENT_INT8 ||
       desc->b_element == MTLC_TENSOR_ELEMENT_UINT8) &&
      desc->accumulator_element == MTLC_TENSOR_ELEMENT_INT32 &&
      desc->overflow == MTLC_TENSOR_OVERFLOW_WRAP;
  int fp8_family = dense && unscaled && logical &&
      (desc->a_element == MTLC_TENSOR_ELEMENT_FLOAT8_E4M3 ||
       desc->a_element == MTLC_TENSOR_ELEMENT_FLOAT8_E5M2) &&
      (desc->b_element == MTLC_TENSOR_ELEMENT_FLOAT8_E4M3 ||
       desc->b_element == MTLC_TENSOR_ELEMENT_FLOAT8_E5M2) &&
      desc->accumulator_element == MTLC_TENSOR_ELEMENT_FLOAT32;
  int scaled_narrow_family =
      dense && !unscaled &&
      ptx_tensor_matmul_narrow_bits(desc->a_element) != 0 &&
      ptx_tensor_matmul_narrow_bits(desc->b_element) != 0 &&
      desc->accumulator_element == MTLC_TENSOR_ELEMENT_FLOAT32;
  int sparse_family = 0;
  if (!dense) {
    PtxMmaProfile sparse_profile;
    char reason[256] = {0};
    int selected = ptx_select_mma_profile(
        fn, desc, &sparse_profile, reason, sizeof(reason));
    if (!selected || !ptx_mma_profile_is_sparse(&sparse_profile)) {
      fn_error(fn,
               "PTX tensor_matmul cannot compose an exact structured-2:4 edge path with its native interior: %s",
               selected ? "selected profile is not structured-sparse" : reason);
      return 0;
    }
    sparse_family = 1;
  }
  if (!f16_family && !f64_family && !i8_family && !fp8_family &&
      !scaled_narrow_family && !sparse_family) {
    fn_error(fn,
             "PTX tensor_matmul exact edge lowering currently supports dense f16/bf16->f32, canonical structured-2:4 matching f16/bf16->f32, unscaled e4m3/e5m2->f32, block-scaled FP8/FP6/FP4->f32, f64, and i8/u8->i32-wrap families; TF32, reduced-precision accumulators, unsupported sparse/scale profiles, and saturating integer tails are rejected");
    return 0;
  }
  if (scaled_narrow_family) {
    PtxMmaProfile scaled_profile;
    char reason[256];
    if (!ptx_select_mma_profile(fn, desc, &scaled_profile, reason,
                                sizeof(reason))) {
      fn_error(fn,
               "PTX tensor_matmul cannot compose an exact scaled edge path with its native interior: %s",
               reason);
      return 0;
    }
    if (scaled_profile.kind != PTX_MMA_MXF8F6F4 &&
        scaled_profile.kind != PTX_MMA_MXFP4 &&
        scaled_profile.kind != PTX_MMA_NVFP4) {
      fn_error(fn,
               "PTX tensor_matmul scaled edge lowering requires a block-scaled FP8/FP6/FP4 native profile");
      return 0;
    }
    if (desc->a_scale_leading_dimension == 0 ||
        desc->b_scale_leading_dimension == 0) {
      fn_error(fn,
               "PTX tensor_matmul block scales require explicit whole-matrix A/B scale leading dimensions because runtime problem K determines the dense minimum");
      return 0;
    }
  }
  if ((desc->a_element == MTLC_TENSOR_ELEMENT_FLOAT16 ||
       desc->b_element == MTLC_TENSOR_ELEMENT_FLOAT16) &&
      (fn->target_arch < 53 || fn->isa_major < 4 ||
       (fn->isa_major == 4 && fn->isa_minor < 2))) {
    fn_error(fn,
             "PTX tensor_matmul f16 edge conversion requires PTX 4.2 and sm_53 or newer");
    return 0;
  }
  if ((desc->a_element == MTLC_TENSOR_ELEMENT_BFLOAT16 ||
       desc->b_element == MTLC_TENSOR_ELEMENT_BFLOAT16) &&
      (fn->target_arch < 80 || fn->isa_major < 7 ||
       (fn->isa_major == 7 && fn->isa_minor < 1))) {
    fn_error(fn,
             "PTX tensor_matmul bf16 edge conversion requires PTX 7.1 and sm_80 or newer");
    return 0;
  }
  if ((desc->a_element == MTLC_TENSOR_ELEMENT_FLOAT8_E4M3 ||
       desc->a_element == MTLC_TENSOR_ELEMENT_FLOAT8_E5M2 ||
       desc->b_element == MTLC_TENSOR_ELEMENT_FLOAT8_E4M3 ||
       desc->b_element == MTLC_TENSOR_ELEMENT_FLOAT8_E5M2) &&
      (fn->target_arch < 89 || fn->isa_major < 8 ||
       (fn->isa_major == 8 && fn->isa_minor < 1))) {
    fn_error(fn,
             "PTX tensor_matmul FP8 edge conversion requires PTX 8.1 and sm_89 or newer");
    return 0;
  }
  return 1;
}

static int ptx_tensor_matmul_address(
    PtxFn *fn, const char *base, const char *row, const char *column,
    const char *leading_dimension, MtlcTensorLayout layout,
    unsigned element_bytes, char address[24]) {
  if (!fn || !base || !row || !column || !leading_dimension || !address ||
      (layout != MTLC_TENSOR_LAYOUT_ROW_MAJOR &&
       layout != MTLC_TENSOR_LAYOUT_COLUMN_MAJOR) ||
      (element_bytes != 1 && element_bytes != 2 && element_bytes != 4 &&
       element_bytes != 8))
    return -1;
  const char *major =
      layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR ? row : column;
  const char *minor =
      layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR ? column : row;
  char linear[24], byte_offset[24];
  reg_name(PC_B64, new_reg(fn, PC_B64), linear);
  sb_printf(&fn->body, "\tmul.lo.u64 %s, %s, %s;\n", linear, major,
            leading_dimension);
  sb_printf(&fn->body, "\tadd.u64 %s, %s, %s;\n", linear, linear,
            minor);
  reg_name(PC_B64, new_reg(fn, PC_B64), byte_offset);
  if (element_bytes == 1)
    sb_printf(&fn->body, "\tmov.u64 %s, %s;\n", byte_offset, linear);
  else
    sb_printf(&fn->body, "\tshl.b64 %s, %s, %u;\n", byte_offset,
              linear, element_bytes == 2 ? 1u : element_bytes == 4 ? 2u : 3u);
  int address_index = new_reg(fn, PC_B64);
  reg_name(PC_B64, address_index, address);
  sb_printf(&fn->body, "\tadd.u64 %s, %s, %s;\n", address, base,
            byte_offset);
  return address_index;
}

static void ptx_tensor_matmul_load_input(PtxFn *fn,
                                         MtlcTensorElement element,
                                         const char *space,
                                         const char *address,
                                         const char *value);

static int ptx_tensor_matmul_storage_address(
    PtxFn *fn, const char *base, const char *row, const char *column,
    const char *leading_dimension, MtlcTensorLayout layout,
    MtlcTensorElement element, MtlcTensorPacking packing, char address[24],
    char aligned[24]) {
  unsigned bits = ptx_tensor_matmul_narrow_bits(element);
  reg_name(PC_PRED, new_reg(fn, PC_PRED), aligned);
  if (packing != MTLC_TENSOR_PACKING_DENSE_SUBBYTE || bits >= 8) {
    sb_printf(&fn->body, "\tmov.pred %s, 1;\n", aligned);
    return ptx_tensor_matmul_address(
        fn, base, row, column, leading_dimension, layout,
        (unsigned)ptx_tensor_matmul_element_bytes(element), address);
  }
  if ((bits != 4 && bits != 6) ||
      (layout != MTLC_TENSOR_LAYOUT_ROW_MAJOR &&
       layout != MTLC_TENSOR_LAYOUT_COLUMN_MAJOR))
    return -1;

  const char *major =
      layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR ? row : column;
  const char *minor =
      layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR ? column : row;
  char linear[24], bit_index[24], byte_offset[24], remainder[24];
  reg_name(PC_B64, new_reg(fn, PC_B64), linear);
  sb_printf(&fn->body, "\tmul.lo.u64 %s, %s, %s;\n", linear, major,
            leading_dimension);
  sb_printf(&fn->body, "\tadd.u64 %s, %s, %s;\n", linear, linear, minor);
  reg_name(PC_B64, new_reg(fn, PC_B64), bit_index);
  sb_printf(&fn->body, "\tmul.lo.u64 %s, %s, %u;\n", bit_index, linear,
            bits);
  reg_name(PC_B64, new_reg(fn, PC_B64), remainder);
  sb_printf(&fn->body, "\tand.b64 %s, %s, 7;\n", remainder, bit_index);
  sb_printf(&fn->body, "\tsetp.eq.u64 %s, %s, 0;\n", aligned, remainder);
  reg_name(PC_B64, new_reg(fn, PC_B64), byte_offset);
  sb_printf(&fn->body, "\tshr.u64 %s, %s, 3;\n", byte_offset, bit_index);
  int address_index = new_reg(fn, PC_B64);
  reg_name(PC_B64, address_index, address);
  sb_printf(&fn->body, "\tadd.u64 %s, %s, %s;\n", address, base,
            byte_offset);
  return address_index;
}

static void ptx_tensor_matmul_decode_narrow(PtxFn *fn,
                                             MtlcTensorElement element,
                                             const char *storage,
                                             const char *value) {
  if (element == MTLC_TENSOR_ELEMENT_FLOAT8_E4M3 ||
      element == MTLC_TENSOR_ELEMENT_FLOAT8_E5M2) {
    char packed_storage[24], packed_f16[24];
    reg_name(PC_B16, new_reg(fn, PC_B16), packed_storage);
    reg_name(PC_B32, new_reg(fn, PC_B32), packed_f16);
    sb_printf(&fn->body, "\tcvt.u16.u32 %s, %s;\n", packed_storage,
              storage);
    sb_printf(&fn->body, "\tcvt.rn.f16x2.%sx2 %s, %s;\n",
              element == MTLC_TENSOR_ELEMENT_FLOAT8_E4M3 ? "e4m3"
                                                         : "e5m2",
              packed_f16, packed_storage);
    sb_printf(&fn->body, "\tcvt.f32.f16 %s, %s;\n", value, packed_f16);
    return;
  }

  unsigned bits = ptx_tensor_matmul_narrow_bits(element);
  unsigned exponent_bits = element == MTLC_TENSOR_ELEMENT_FLOAT6_E3M2 ? 3 : 2;
  unsigned mantissa_bits = element == MTLC_TENSOR_ELEMENT_FLOAT4_E2M1
                               ? 1
                               : (element == MTLC_TENSOR_ELEMENT_FLOAT6_E3M2
                                      ? 2
                                      : 3);
  unsigned bias = element == MTLC_TENSOR_ELEMENT_FLOAT6_E3M2 ? 3 : 1;
  if ((bits != 4 && bits != 6) || exponent_bits + mantissa_bits + 1 != bits) {
    fn_error(fn, "PTX tensor_matmul cannot decode this narrow input format");
    return;
  }

  char exponent[24], mantissa[24], normal_significand[24];
  char significand[24], normal_scale_exponent[24], scale_exponent[24];
  char scale_bits[24], scale[24], sign[24], exponent_zero[24], negative[24];
  reg_name(PC_B32, new_reg(fn, PC_B32), exponent);
  sb_printf(&fn->body, "\tshr.u32 %s, %s, %u;\n", exponent, storage,
            mantissa_bits);
  sb_printf(&fn->body, "\tand.b32 %s, %s, %u;\n", exponent, exponent,
            (1u << exponent_bits) - 1u);
  reg_name(PC_B32, new_reg(fn, PC_B32), mantissa);
  sb_printf(&fn->body, "\tand.b32 %s, %s, %u;\n", mantissa, storage,
            (1u << mantissa_bits) - 1u);
  reg_name(PC_PRED, new_reg(fn, PC_PRED), exponent_zero);
  sb_printf(&fn->body, "\tsetp.eq.u32 %s, %s, 0;\n", exponent_zero,
            exponent);
  reg_name(PC_B32, new_reg(fn, PC_B32), normal_significand);
  sb_printf(&fn->body, "\tadd.u32 %s, %s, %u;\n", normal_significand,
            mantissa, 1u << mantissa_bits);
  reg_name(PC_B32, new_reg(fn, PC_B32), significand);
  sb_printf(&fn->body, "\tselp.u32 %s, %s, %s, %s;\n", significand,
            mantissa, normal_significand, exponent_zero);
  reg_name(PC_B32, new_reg(fn, PC_B32), normal_scale_exponent);
  sb_printf(&fn->body, "\tadd.u32 %s, %s, %u;\n", normal_scale_exponent,
            exponent, 127u - bias - mantissa_bits);
  reg_name(PC_B32, new_reg(fn, PC_B32), scale_exponent);
  sb_printf(&fn->body, "\tselp.u32 %s, %u, %s, %s;\n", scale_exponent,
            127u + 1u - bias - mantissa_bits, normal_scale_exponent,
            exponent_zero);
  reg_name(PC_B32, new_reg(fn, PC_B32), scale_bits);
  sb_printf(&fn->body, "\tshl.b32 %s, %s, 23;\n", scale_bits,
            scale_exponent);
  reg_name(PC_F32, new_reg(fn, PC_F32), scale);
  sb_printf(&fn->body, "\tmov.b32 %s, %s;\n", scale, scale_bits);
  sb_printf(&fn->body, "\tcvt.rn.f32.u32 %s, %s;\n", value, significand);
  sb_printf(&fn->body, "\tmul.rn.f32 %s, %s, %s;\n", value, value, scale);
  reg_name(PC_B32, new_reg(fn, PC_B32), sign);
  sb_printf(&fn->body, "\tand.b32 %s, %s, %u;\n", sign, storage,
            1u << (bits - 1u));
  reg_name(PC_PRED, new_reg(fn, PC_PRED), negative);
  sb_printf(&fn->body, "\tsetp.ne.u32 %s, %s, 0;\n", negative, sign);
  sb_printf(&fn->body, "\t@%s neg.f32 %s, %s;\n", negative, value, value);
}

static int ptx_tensor_matmul_load_operand(
    PtxFn *fn, MtlcTensorElement element, MtlcTensorPacking packing,
    const char *base, const char *space, const char *row, const char *column,
    const char *leading_dimension, MtlcTensorLayout layout,
    const char *value) {
  unsigned bits = ptx_tensor_matmul_narrow_bits(element);
  if (!bits) {
    char address[24];
    if (ptx_tensor_matmul_address(
            fn, base, row, column, leading_dimension, layout,
            (unsigned)ptx_tensor_matmul_element_bytes(element), address) < 0)
      return 0;
    ptx_tensor_matmul_load_input(fn, element, space, address, value);
    return 1;
  }

  char storage[24];
  reg_name(PC_B32, new_reg(fn, PC_B32), storage);
  if (packing != MTLC_TENSOR_PACKING_DENSE_SUBBYTE || bits == 8) {
    char address[24];
    if (ptx_tensor_matmul_address(fn, base, row, column, leading_dimension,
                                  layout, 1, address) < 0)
      return 0;
    sb_printf(&fn->body, "\tld%s.u8 %s, [%s];\n", space, storage,
              address);
  } else {
    const char *major =
        layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR ? row : column;
    const char *minor =
        layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR ? column : row;
    char linear[24], bit_index[24], byte_index[24], bit_shift64[24];
    char bit_shift[24], address[24], next_address[24];
    char low[24], high[24], word[24], crosses[24];
    reg_name(PC_B64, new_reg(fn, PC_B64), linear);
    sb_printf(&fn->body, "\tmul.lo.u64 %s, %s, %s;\n", linear, major,
              leading_dimension);
    sb_printf(&fn->body, "\tadd.u64 %s, %s, %s;\n", linear, linear,
              minor);
    reg_name(PC_B64, new_reg(fn, PC_B64), bit_index);
    sb_printf(&fn->body, "\tmul.lo.u64 %s, %s, %u;\n", bit_index,
              linear, bits);
    reg_name(PC_B64, new_reg(fn, PC_B64), byte_index);
    sb_printf(&fn->body, "\tshr.u64 %s, %s, 3;\n", byte_index,
              bit_index);
    reg_name(PC_B64, new_reg(fn, PC_B64), bit_shift64);
    sb_printf(&fn->body, "\tand.b64 %s, %s, 7;\n", bit_shift64,
              bit_index);
    reg_name(PC_B32, new_reg(fn, PC_B32), bit_shift);
    sb_printf(&fn->body, "\tcvt.u32.u64 %s, %s;\n", bit_shift,
              bit_shift64);
    reg_name(PC_B64, new_reg(fn, PC_B64), address);
    sb_printf(&fn->body, "\tadd.u64 %s, %s, %s;\n", address, base,
              byte_index);
    reg_name(PC_B32, new_reg(fn, PC_B32), low);
    sb_printf(&fn->body, "\tld%s.u8 %s, [%s];\n", space, low, address);
    reg_name(PC_B32, new_reg(fn, PC_B32), high);
    sb_printf(&fn->body, "\tmov.u32 %s, 0;\n", high);
    reg_name(PC_PRED, new_reg(fn, PC_PRED), crosses);
    sb_printf(&fn->body, "\tsetp.gt.u32 %s, %s, %u;\n", crosses,
              bit_shift, 8u - bits);
    reg_name(PC_B64, new_reg(fn, PC_B64), next_address);
    sb_printf(&fn->body, "\tadd.u64 %s, %s, 1;\n", next_address,
              address);
    sb_printf(&fn->body, "\t@%s ld%s.u8 %s, [%s];\n", crosses, space,
              high, next_address);
    reg_name(PC_B32, new_reg(fn, PC_B32), word);
    sb_printf(&fn->body, "\tshl.b32 %s, %s, 8;\n", word, high);
    sb_printf(&fn->body, "\tor.b32 %s, %s, %s;\n", word, word, low);
    sb_printf(&fn->body, "\tshr.u32 %s, %s, %s;\n", storage, word,
              bit_shift);
    sb_printf(&fn->body, "\tand.b32 %s, %s, %u;\n", storage, storage,
              (1u << bits) - 1u);
  }
  ptx_tensor_matmul_decode_narrow(fn, element, storage, value);
  return !fn->error;
}

static int ptx_tensor_matmul_load_sparse_a(
    PtxFn *fn, const MtlcTensorMmaDesc *desc, const char *a_base,
    const char *a_space, const char *metadata_base,
    const char *metadata_space, const char *row, const char *q,
    const char *a_leading_dimension, const char *metadata_stride,
    const char *value) {
  if (!fn || !desc || !a_base || !a_space || !metadata_base ||
      !metadata_space || !row || !q || !a_leading_dimension ||
      !metadata_stride || !value ||
      desc->sparsity != MTLC_TENSOR_SPARSITY_STRUCTURED_2_TO_4 ||
      (desc->a_element != MTLC_TENSOR_ELEMENT_FLOAT16 &&
       desc->a_element != MTLC_TENSOR_ELEMENT_BFLOAT16))
    return 0;

  char group[24], position64[24], position[24], metadata_address[24];
  char mask[24], count[24], selected_bit[24], selected_bits[24];
  char lower_bits[24], rank_bits[24], rank[24], rank64[24];
  char compressed_column[24], a_address[24], storage[24];
  char invalid[24], selected[24];
  reg_name(PC_B64, new_reg(fn, PC_B64), group);
  reg_name(PC_B64, new_reg(fn, PC_B64), position64);
  reg_name(PC_B32, new_reg(fn, PC_B32), position);
  sb_printf(&fn->body,
            "\tshr.u64 %s, %s, 2;\n"
            "\tand.b64 %s, %s, 3;\n"
            "\tcvt.u32.u64 %s, %s;\n",
            group, q, position64, q, position, position64);
  if (ptx_tensor_matmul_address(
          fn, metadata_base, row, group, metadata_stride,
          MTLC_TENSOR_LAYOUT_ROW_MAJOR, 1, metadata_address) < 0)
    return 0;
  reg_name(PC_B32, new_reg(fn, PC_B32), mask);
  reg_name(PC_B32, new_reg(fn, PC_B32), count);
  reg_name(PC_PRED, new_reg(fn, PC_PRED), invalid);
  sb_printf(&fn->body,
            "\tld%s.u8 %s, [%s];\n"
            "\tand.b32 %s, %s, 15;\n"
            "\tpopc.b32 %s, %s;\n"
            "\tsetp.ne.u32 %s, %s, 2;\n"
            "\t@%s mov.u32 %s, 3;\n",
            metadata_space, mask, metadata_address, mask, mask, count, mask,
            invalid, count, invalid, mask);

  reg_name(PC_B32, new_reg(fn, PC_B32), selected_bit);
  reg_name(PC_B32, new_reg(fn, PC_B32), selected_bits);
  reg_name(PC_B32, new_reg(fn, PC_B32), lower_bits);
  reg_name(PC_B32, new_reg(fn, PC_B32), rank_bits);
  reg_name(PC_B32, new_reg(fn, PC_B32), rank);
  reg_name(PC_PRED, new_reg(fn, PC_PRED), selected);
  sb_printf(&fn->body,
            "\tshl.b32 %s, 1, %s;\n"
            "\tand.b32 %s, %s, %s;\n"
            "\tsetp.ne.u32 %s, %s, 0;\n"
            "\tsub.u32 %s, %s, 1;\n"
            "\tand.b32 %s, %s, %s;\n"
            "\tpopc.b32 %s, %s;\n",
            selected_bit, position, selected_bits, mask, selected_bit,
            selected, selected_bits, lower_bits, selected_bit, rank_bits, mask,
            lower_bits, rank, rank_bits);
  reg_name(PC_B64, new_reg(fn, PC_B64), rank64);
  reg_name(PC_B64, new_reg(fn, PC_B64), compressed_column);
  sb_printf(&fn->body,
            "\tcvt.u64.u32 %s, %s;\n"
            "\tshl.b64 %s, %s, 1;\n"
            "\tadd.u64 %s, %s, %s;\n",
            rank64, rank, compressed_column, group, compressed_column,
            compressed_column, rank64);

  const char *storage_row = desc->transpose_a ? compressed_column : row;
  const char *storage_column = desc->transpose_a ? row : compressed_column;
  if (ptx_tensor_matmul_address(
          fn, a_base, storage_row, storage_column, a_leading_dimension,
          desc->a_layout, 2, a_address) < 0)
    return 0;
  reg_name(PC_B16, new_reg(fn, PC_B16), storage);
  sb_printf(&fn->body,
            "\tmov.b16 %s, 0;\n"
            "\t@%s ld%s.b16 %s, [%s];\n"
            "\tcvt.f32.%s %s, %s;\n",
            storage, selected, a_space, storage, a_address,
            desc->a_element == MTLC_TENSOR_ELEMENT_FLOAT16 ? "f16" : "bf16",
            value, storage);
  return !fn->error;
}

static void ptx_tensor_matmul_decode_scale(PtxFn *fn,
                                            MtlcTensorElement element,
                                            const char *storage,
  const char *value) {
  if (element == MTLC_TENSOR_ELEMENT_SCALE_UE4M3) {
    char unsigned_storage[24];
    reg_name(PC_B32, new_reg(fn, PC_B32), unsigned_storage);
    sb_printf(&fn->body, "\tand.b32 %s, %s, 127;\n", unsigned_storage,
              storage);
    ptx_tensor_matmul_decode_narrow(
        fn, MTLC_TENSOR_ELEMENT_FLOAT8_E4M3, unsigned_storage, value);
    return;
  }
  if (element != MTLC_TENSOR_ELEMENT_SCALE_UE8M0) {
    fn_error(fn, "PTX tensor_matmul cannot decode this block-scale format");
    return;
  }
  char bits[24], zero[24], nan[24];
  reg_name(PC_B32, new_reg(fn, PC_B32), bits);
  reg_name(PC_PRED, new_reg(fn, PC_PRED), zero);
  reg_name(PC_PRED, new_reg(fn, PC_PRED), nan);
  sb_printf(&fn->body, "\tsetp.eq.u32 %s, %s, 0;\n", zero, storage);
  sb_printf(&fn->body, "\tsetp.eq.u32 %s, %s, 255;\n", nan, storage);
  sb_printf(&fn->body, "\tshl.b32 %s, %s, 23;\n", bits, storage);
  sb_printf(&fn->body, "\t@%s mov.u32 %s, 4194304;\n", zero, bits);
  sb_printf(&fn->body, "\t@%s mov.u32 %s, 2143289344;\n", nan, bits);
  sb_printf(&fn->body, "\tmov.b32 %s, %s;\n", value, bits);
}

static int ptx_tensor_matmul_load_scale(
    PtxFn *fn, const MtlcTensorMmaDesc *desc, int a_scale,
    const char *base, const char *space, const char *matrix_coordinate,
    const char *q, const char *leading_dimension, const char *value) {
  MtlcTensorScaleMode mode =
      a_scale ? desc->a_scale_mode : desc->b_scale_mode;
  MtlcTensorElement element =
      a_scale ? desc->a_scale_element : desc->b_scale_element;
  unsigned shift = mode == MTLC_TENSOR_SCALE_BLOCK_16
                       ? 4
                       : mode == MTLC_TENSOR_SCALE_BLOCK_32 ? 5 : 0;
  if (!shift) return 0;
  char chunk[24], address[24], storage[24];
  reg_name(PC_B64, new_reg(fn, PC_B64), chunk);
  sb_printf(&fn->body, "\tshr.u64 %s, %s, %u;\n", chunk, q, shift);
  const char *row = a_scale ? matrix_coordinate : chunk;
  const char *column = a_scale ? chunk : matrix_coordinate;
  MtlcTensorLayout layout = a_scale ? MTLC_TENSOR_LAYOUT_ROW_MAJOR
                                    : MTLC_TENSOR_LAYOUT_COLUMN_MAJOR;
  if (ptx_tensor_matmul_address(fn, base, row, column, leading_dimension,
                                layout, 1, address) < 0)
    return 0;
  reg_name(PC_B32, new_reg(fn, PC_B32), storage);
  sb_printf(&fn->body, "\tld%s.u8 %s, [%s];\n", space, storage, address);
  ptx_tensor_matmul_decode_scale(fn, element, storage, value);
  return !fn->error;
}

static void ptx_tensor_matmul_load_input(PtxFn *fn,
                                         MtlcTensorElement element,
                                         const char *space,
                                         const char *address,
                                         const char *value) {
  if (element == MTLC_TENSOR_ELEMENT_FLOAT16 ||
      element == MTLC_TENSOR_ELEMENT_BFLOAT16) {
    char storage[24];
    reg_name(PC_B16, new_reg(fn, PC_B16), storage);
    sb_printf(&fn->body, "\tld%s.b16 %s, [%s];\n", space, storage,
              address);
    sb_printf(&fn->body, "\tcvt.f32.%s %s, %s;\n",
              element == MTLC_TENSOR_ELEMENT_FLOAT16 ? "f16" : "bf16",
              value, storage);
  } else if (element == MTLC_TENSOR_ELEMENT_FLOAT8_E4M3 ||
             element == MTLC_TENSOR_ELEMENT_FLOAT8_E5M2) {
    char storage[24], packed_f16[24];
    reg_name(PC_B16, new_reg(fn, PC_B16), storage);
    reg_name(PC_B32, new_reg(fn, PC_B32), packed_f16);
    sb_printf(&fn->body, "\tld%s.u8 %s, [%s];\n", space, storage,
              address);
    sb_printf(&fn->body, "\tcvt.rn.f16x2.%sx2 %s, %s;\n",
              element == MTLC_TENSOR_ELEMENT_FLOAT8_E4M3 ? "e4m3"
                                                         : "e5m2",
              packed_f16, storage);
    sb_printf(&fn->body, "\tcvt.f32.f16 %s, %s;\n", value, packed_f16);
  } else if (element == MTLC_TENSOR_ELEMENT_FLOAT64) {
    sb_printf(&fn->body, "\tld%s.f64 %s, [%s];\n", space, value,
              address);
  } else if (element == MTLC_TENSOR_ELEMENT_INT8) {
    sb_printf(&fn->body, "\tld%s.s8 %s, [%s];\n", space, value,
              address);
  } else {
    sb_printf(&fn->body, "\tld%s.u8 %s, [%s];\n", space, value,
              address);
  }
}

static void ptx_tensor_matmul_load_accumulator(
    PtxFn *fn, MtlcTensorElement element, const char *space,
    const char *address, const char *value) {
  const char *type = element == MTLC_TENSOR_ELEMENT_FLOAT64
                         ? "f64"
                     : element == MTLC_TENSOR_ELEMENT_FLOAT32 ? "f32"
                                                               : "s32";
  sb_printf(&fn->body, "\tld%s.%s %s, [%s];\n", space, type, value,
            address);
}

static void ptx_tensor_matmul_store_result(PtxFn *fn,
                                           MtlcTensorElement element,
                                           const char *space,
                                           const char *address,
                                           const char *value) {
  const char *type = element == MTLC_TENSOR_ELEMENT_FLOAT64
                         ? "f64"
                     : element == MTLC_TENSOR_ELEMENT_FLOAT32 ? "f32"
                                                               : "b32";
  sb_printf(&fn->body, "\tst%s.%s [%s], %s;\n", space, type, address,
            value);
}

static int ptx_tensor_matmul_stride64(PtxFn *fn,
                                      const IRInstruction *in,
                                      size_t per_tile,
                                      char strides[4][24]) {
  uint32_t static_strides[4] = {
      IR_TENSOR_MMA(in).a_leading_dimension,
      IR_TENSOR_MMA(in).b_leading_dimension,
      IR_TENSOR_MMA(in).c_leading_dimension,
      IR_TENSOR_MMA(in).d_leading_dimension};
  size_t argument =
      4u +
      (IR_TENSOR_MMA(in).sparsity != MTLC_TENSOR_SPARSITY_DENSE ? 1u : 0u) +
      (IR_TENSOR_MMA(in).a_scale_mode != MTLC_TENSOR_SCALE_NONE ? 1u : 0u) +
      (IR_TENSOR_MMA(in).b_scale_mode != MTLC_TENSOR_SCALE_NONE ? 1u : 0u);
  for (size_t i = 0; i < 4; i++) {
    if (static_strides[i] == 0) {
      if (argument >= per_tile) return 0;
      use_as(fn, &in->arguments[argument++], PC_B64, strides[i]);
    } else {
      reg_name(PC_B64, new_reg(fn, PC_B64), strides[i]);
      sb_printf(&fn->body, "\tmov.u64 %s, %u;\n", strides[i],
                static_strides[i]);
    }
  }
  return argument == per_tile;
}

static void ptx_emit_tensor_matmul_scalar(
    PtxFn *fn, const IRInstruction *in, char bases[6][24],
    const char *spaces[6], char strides[4][24],
    char scale_strides[2][24], const char *metadata_stride,
    const char *row_origin, const char *column_origin,
    const char *problem_m, const char *problem_n, const char *problem_k,
    const char *start_k, int initialize_from_d, const char *lane_rank,
    const char *participants, const char *mask, unsigned long long label_id,
    const char *phase) {
  const MtlcTensorMmaDesc *desc = &IR_TENSOR_MMA(in);
  PtxClass compute_class =
      desc->accumulator_element == MTLC_TENSOR_ELEMENT_FLOAT64
          ? PC_F64
      : desc->accumulator_element == MTLC_TENSOR_ELEMENT_FLOAT32
          ? PC_F32
          : PC_B32;
  char linear[24], row_local[24], column_local[24];
  char row_local64[24], column_local64[24], row[24], column[24];
  char q[24], done[24], row_oob[24], column_oob[24];
  char c_address[24], d_address[24];
  char a_value[24], b_value[24], accumulator[24];
  char a_scale_value[24] = {0}, b_scale_value[24] = {0};
  int scaled = desc->a_scale_mode != MTLC_TENSOR_SCALE_NONE;
  int sparse = desc->sparsity != MTLC_TENSOR_SPARSITY_DENSE;
  reg_name(PC_B32, new_reg(fn, PC_B32), linear);
  reg_name(PC_B32, new_reg(fn, PC_B32), row_local);
  reg_name(PC_B32, new_reg(fn, PC_B32), column_local);
  reg_name(PC_B64, new_reg(fn, PC_B64), row_local64);
  reg_name(PC_B64, new_reg(fn, PC_B64), column_local64);
  reg_name(PC_B64, new_reg(fn, PC_B64), row);
  reg_name(PC_B64, new_reg(fn, PC_B64), column);
  reg_name(PC_B64, new_reg(fn, PC_B64), q);
  reg_name(PC_PRED, new_reg(fn, PC_PRED), done);
  reg_name(PC_PRED, new_reg(fn, PC_PRED), row_oob);
  reg_name(PC_PRED, new_reg(fn, PC_PRED), column_oob);
  reg_name(compute_class, new_reg(fn, compute_class), accumulator);
  reg_name(compute_class, new_reg(fn, compute_class), a_value);
  reg_name(compute_class, new_reg(fn, compute_class), b_value);
  if (scaled) {
    reg_name(PC_F32, new_reg(fn, PC_F32), a_scale_value);
    reg_name(PC_F32, new_reg(fn, PC_F32), b_scale_value);
  }
  sb_printf(&fn->body,
            "\t// mtlc.tensor_matmul cooperative-%s exact M/N/K edge replay\n"
            "\tmov.u32 %s, %s;\n"
            "mtlc_tensor_matmul_%llu_%s_output:\n"
            "\tsetp.ge.u32 %s, %s, %u;\n"
            "\t@%s bra mtlc_tensor_matmul_%llu_%s_done;\n"
            "\tdiv.u32 %s, %s, %u;\n"
            "\trem.u32 %s, %s, %u;\n"
            "\tcvt.u64.u32 %s, %s;\n"
            "\tcvt.u64.u32 %s, %s;\n"
            "\tadd.u64 %s, %s, %s;\n"
            "\tadd.u64 %s, %s, %s;\n"
            "\tsetp.ge.u64 %s, %s, %s;\n"
            "\t@%s bra mtlc_tensor_matmul_%llu_%s_next;\n"
            "\tsetp.ge.u64 %s, %s, %s;\n"
            "\t@%s bra mtlc_tensor_matmul_%llu_%s_next;\n",
            phase, linear, lane_rank, label_id, phase, done, linear,
            (unsigned)((uint32_t)desc->m * (uint32_t)desc->n), done,
            label_id, phase, row_local, linear, (unsigned)desc->n,
            column_local, linear, (unsigned)desc->n, row_local64, row_local,
            column_local64, column_local, row, row_origin, row_local64,
            column, column_origin, column_local64, row_oob, row, problem_m,
            row_oob, label_id, phase, column_oob, column, problem_n,
            column_oob, label_id, phase);

  int c_bytes = ptx_tensor_matmul_element_bytes(
      initialize_from_d ? desc->result_element : desc->accumulator_element);
  if (ptx_tensor_matmul_address(
          fn, initialize_from_d ? bases[3] : bases[2], row, column,
          initialize_from_d ? strides[3] : strides[2],
          initialize_from_d ? desc->d_layout : desc->c_layout,
          (unsigned)c_bytes, c_address) < 0) {
    fn_error(fn, "PTX tensor_matmul cannot address its accumulator matrix");
    return;
  }
  ptx_tensor_matmul_load_accumulator(
      fn, initialize_from_d ? desc->result_element
                            : desc->accumulator_element,
      initialize_from_d ? spaces[3] : spaces[2], c_address, accumulator);
  sb_printf(&fn->body,
            "\tmov.u64 %s, %s;\n"
            "mtlc_tensor_matmul_%llu_%s_k:\n"
            "\tsetp.ge.u64 %s, %s, %s;\n"
            "\t@%s bra mtlc_tensor_matmul_%llu_%s_store;\n",
            q, start_k, label_id, phase, done, q, problem_k, done, label_id,
            phase);
  const char *a_row = desc->transpose_a ? q : row;
  const char *a_column = desc->transpose_a ? row : q;
  const char *b_row = desc->transpose_b ? column : q;
  const char *b_column = desc->transpose_b ? q : column;
  int loaded_a = sparse
                     ? ptx_tensor_matmul_load_sparse_a(
                           fn, desc, bases[0], spaces[0], bases[4], spaces[4],
                           row, q, strides[0], metadata_stride, a_value)
                     : ptx_tensor_matmul_load_operand(
                           fn, desc->a_element, desc->a_packing, bases[0],
                           spaces[0], a_row, a_column, strides[0],
                           desc->a_layout, a_value);
  if (!loaded_a ||
      !ptx_tensor_matmul_load_operand(
          fn, desc->b_element, desc->b_packing, bases[1], spaces[1], b_row,
          b_column, strides[1], desc->b_layout, b_value)) {
    fn_error(fn, "PTX tensor_matmul cannot address A/B edge operands");
    return;
  }
  if (scaled) {
    if (!ptx_tensor_matmul_load_scale(
            fn, desc, 1, bases[4], spaces[4], row, q, scale_strides[0],
            a_scale_value) ||
        !ptx_tensor_matmul_load_scale(
            fn, desc, 0, bases[5], spaces[5], column, q,
            scale_strides[1], b_scale_value)) {
      fn_error(fn, "PTX tensor_matmul cannot address A/B block scales");
      return;
    }
    sb_printf(&fn->body, "\tmul.rn.f32 %s, %s, %s;\n", a_value,
              a_value, a_scale_value);
    sb_printf(&fn->body, "\tmul.rn.f32 %s, %s, %s;\n", b_value,
              b_value, b_scale_value);
  }
  if (compute_class == PC_F64) {
    sb_printf(&fn->body, "\tfma.rn.f64 %s, %s, %s, %s;\n", accumulator,
              a_value, b_value, accumulator);
  } else if (compute_class == PC_F32) {
    sb_printf(&fn->body, "\tfma.rn.f32 %s, %s, %s, %s;\n", accumulator,
              a_value, b_value, accumulator);
  } else {
    sb_printf(&fn->body, "\tmad.lo.s32 %s, %s, %s, %s;\n", accumulator,
              a_value, b_value, accumulator);
  }
  sb_printf(&fn->body,
            "\tadd.u64 %s, %s, 1;\n"
            "\tbra mtlc_tensor_matmul_%llu_%s_k;\n"
            "mtlc_tensor_matmul_%llu_%s_store:\n",
            q, q, label_id, phase, label_id, phase);
  int d_bytes = ptx_tensor_matmul_element_bytes(desc->result_element);
  if (ptx_tensor_matmul_address(fn, bases[3], row, column, strides[3],
                                desc->d_layout, (unsigned)d_bytes,
                                d_address) < 0) {
    fn_error(fn, "PTX tensor_matmul cannot address D edge results");
    return;
  }
  ptx_tensor_matmul_store_result(fn, desc->result_element, spaces[3],
                                 d_address, accumulator);
  sb_printf(&fn->body,
            "mtlc_tensor_matmul_%llu_%s_next:\n"
            "\tadd.u32 %s, %s, %s;\n"
            "\tbra mtlc_tensor_matmul_%llu_%s_output;\n"
            "mtlc_tensor_matmul_%llu_%s_done:\n"
            "\tbar.warp.sync %s;\n"
            "\tbra mtlc_tensor_matmul_%llu_finish;\n",
            label_id, phase, linear, linear, participants, label_id, phase,
            label_id, phase, mask, label_id);
}

static unsigned ptx_tensor_storage_bits(MtlcTensorElement element) {
  switch (element) {
  case MTLC_TENSOR_ELEMENT_BIT1:
    return 1;
  case MTLC_TENSOR_ELEMENT_INT4:
  case MTLC_TENSOR_ELEMENT_UINT4:
  case MTLC_TENSOR_ELEMENT_FLOAT4_E2M1:
    return 4;
  case MTLC_TENSOR_ELEMENT_TFLOAT32:
    return 32;
  default:
    return (unsigned)ptx_tensor_matmul_element_bytes(element) * 8u;
  }
}

static void ptx_tensor_matmul_wmma_guard(PtxFn *fn, const IRInstruction *in,
                                         int direct_mma, char bases[6][24],
                                         char strides64[4][24],
                                         const char *native) {
  const MtlcTensorMmaDesc *desc = &IR_TENSOR_MMA(in);
  if (direct_mma) {
    return;
  }
  MtlcTensorElement elements[4] = {desc->a_element, desc->b_element,
                                   desc->accumulator_element,
                                   desc->result_element};
  sb_printf(&fn->body,
            "\t// mtlc.tensor_matmul stable-wmma stride and base alignment guard\n");
  for (size_t i = 0; i < 4; i++) {
    unsigned bits = ptx_tensor_storage_bits(elements[i]);
    char remainder[24], aligned[24];
    if (bits == 0 || 128u % bits != 0) continue;
    reg_name(PC_B64, new_reg(fn, PC_B64), remainder);
    reg_name(PC_PRED, new_reg(fn, PC_PRED), aligned);
    sb_printf(&fn->body,
              "\tand.b64 %s, %s, %u;\n"
              "\tsetp.eq.u64 %s, %s, 0;\n"
              "\tand.pred %s, %s, %s;\n"
              "\tand.b64 %s, %s, 31;\n"
              "\tsetp.eq.u64 %s, %s, 0;\n"
              "\tand.pred %s, %s, %s;\n",
              remainder, strides64[i], 128u / bits - 1u, aligned, remainder,
              native, native, aligned, remainder, bases[i], aligned, remainder,
              native, native, aligned);
  }
}

static int ptx_tensor_matmul_bind_pointer(PtxFn *fn, const char *name,
                                          PtxVal descriptor,
                                          int register_index) {
  if (!fn || !name || register_index < 0) return 0;
  descriptor.cls = PC_B64;
  descriptor.idx = register_index;
  descriptor.is_ptr = 1;
  return bind_value(fn, name, descriptor) != NULL;
}

static void ptx_emit_tensor_matmul(PtxFn *fn,
                                   const IRInstruction *in) {
  size_t per_tile = ir_tensor_mma_operand_count(&IR_TENSOR_MMA(in));
  size_t expected = ir_tensor_matmul_operand_count(&IR_TENSOR_MMA(in));
  if (in->op != IR_OP_TENSOR_MATMUL || !per_tile ||
      expected != per_tile + 5u || in->argument_count != expected ||
      !ptx_tensor_matmul_capability(fn, &IR_TENSOR_MMA(in)))
    return;

  int sparse = IR_TENSOR_MMA(in).sparsity != MTLC_TENSOR_SPARSITY_DENSE;
  int scaled = IR_TENSOR_MMA(in).a_scale_mode != MTLC_TENSOR_SCALE_NONE;
  size_t pointer_count = 4u + (sparse ? 1u : scaled ? 2u : 0u);
  char bases[6][24], strides32[4][24], strides64[4][24];
  char scale_strides64[2][24] = {{0}};
  char metadata_stride64[24] = {0}, metadata_stride32[24] = {0};
  const char *spaces[6];
  PtxVal pointer_descs[6];
  for (size_t i = 0; i < pointer_count; i++) {
    pointer_descs[i] = operand_desc(fn, &in->arguments[i]);
    spaces[i] = ptx_wmma_space(pointer_descs[i]);
    if (!pointer_descs[i].is_ptr || !spaces[i]) {
      fn_error(fn,
               "PTX tensor_matmul requires generic/global/workgroup matrix, metadata, and scale pointers");
      return;
    }
    use_as(fn, &in->arguments[i], PC_B64, bases[i]);
  }
  if (!ptx_tensor_stride_registers(fn, in, 0, per_tile, strides32) ||
      !ptx_tensor_matmul_stride64(fn, in, per_tile, strides64)) {
    fn_error(fn, "PTX tensor_matmul has inconsistent leading dimensions");
    return;
  }
  if (scaled) {
    uint32_t scale_strides[2] = {
        IR_TENSOR_MMA(in).a_scale_leading_dimension,
        IR_TENSOR_MMA(in).b_scale_leading_dimension};
    for (size_t i = 0; i < 2; i++) {
      reg_name(PC_B64, new_reg(fn, PC_B64), scale_strides64[i]);
      sb_printf(&fn->body, "\tmov.u64 %s, %u;\n", scale_strides64[i],
                scale_strides[i]);
    }
  }

  char row_origin[24], column_origin[24];
  char problem_m[24], problem_n[24], problem_k[24], zero64[24];
  use_as(fn, &in->arguments[per_tile], PC_B64, row_origin);
  use_as(fn, &in->arguments[per_tile + 1], PC_B64, column_origin);
  use_as(fn, &in->arguments[per_tile + 2], PC_B64, problem_m);
  use_as(fn, &in->arguments[per_tile + 3], PC_B64, problem_n);
  use_as(fn, &in->arguments[per_tile + 4], PC_B64, problem_k);
  reg_name(PC_B64, new_reg(fn, PC_B64), zero64);
  sb_printf(&fn->body, "\tmov.u64 %s, 0;\n", zero64);
  if (sparse) {
    char remainder[24], has_partial_group[24];
    reg_name(PC_B64, new_reg(fn, PC_B64), metadata_stride64);
    reg_name(PC_B64, new_reg(fn, PC_B64), remainder);
    reg_name(PC_PRED, new_reg(fn, PC_PRED), has_partial_group);
    reg_name(PC_B32, new_reg(fn, PC_B32), metadata_stride32);
    sb_printf(&fn->body,
              "\tshr.u64 %s, %s, 2;\n"
              "\tand.b64 %s, %s, 3;\n"
              "\tsetp.ne.u64 %s, %s, 0;\n"
              "\t@%s add.u64 %s, %s, 1;\n"
              "\tcvt.u32.u64 %s, %s;\n",
              metadata_stride64, problem_k, remainder, problem_k,
              has_partial_group, remainder, has_partial_group,
              metadata_stride64, metadata_stride64, metadata_stride32,
              metadata_stride64);
  }

  char mask[24], below[24], active_below[24];
  char lane_rank[24], participants[24];
  reg_name(PC_B32, new_reg(fn, PC_B32), mask);
  reg_name(PC_B32, new_reg(fn, PC_B32), below);
  reg_name(PC_B32, new_reg(fn, PC_B32), active_below);
  reg_name(PC_B32, new_reg(fn, PC_B32), lane_rank);
  reg_name(PC_B32, new_reg(fn, PC_B32), participants);
  sb_printf(&fn->body,
            "\tactivemask.b32 %s;\n"
            "\tmov.u32 %s, %%lanemask_lt;\n"
            "\tand.b32 %s, %s, %s;\n"
            "\tpopc.b32 %s, %s;\n"
            "\tpopc.b32 %s, %s;\n"
            "\tbar.warp.sync %s;\n",
            mask, below, active_below, mask, below, lane_rank, active_below,
            participants, mask, mask);

  unsigned long long label_id = (unsigned long long)fn->call_count++;
  MtlcTensorMmaDesc native_desc = IR_TENSOR_MMA(in);
  if (native_desc.transpose_a) {
    native_desc.a_layout =
        ptx_tensor_matmul_transposed_layout(native_desc.a_layout);
    native_desc.transpose_a = 0;
  }
  if (native_desc.transpose_b) {
    native_desc.b_layout =
        ptx_tensor_matmul_transposed_layout(native_desc.b_layout);
    native_desc.transpose_b = 0;
  }
  PtxWmmaProfile profile;
  PtxMmaProfile direct_profile;
  char native_reason[256];
  int use_direct_mma = 0;
  int have_native =
      ptx_select_wmma_profile(fn, &native_desc, &profile,
                              native_reason, sizeof(native_reason));
  if (!have_native && ptx_tensor_uses_direct_mma(&native_desc)) {
    char direct_reason[256];
    if (ptx_select_mma_profile(fn, &native_desc, &direct_profile,
                               direct_reason, sizeof(direct_reason))) {
      have_native = 1;
      use_direct_mma = 1;
    } else {
      snprintf(native_reason, sizeof(native_reason), "%s", direct_reason);
    }
  }
  int native_peak = 0;
  if (have_native) {
    native_peak =
        use_direct_mma
             ? direct_profile.m_tiles * direct_profile.n_tiles *
                       direct_profile.accumulator_registers +
                   direct_profile.a_registers + direct_profile.b_registers +
                   (sparse ? 1 : scaled ? 2 : 0)
            : profile.m_tiles * profile.n_tiles * profile.d_registers +
                  profile.a_registers + profile.b_registers +
                  profile.c_registers;
    if (native_peak > ptx_tensor_tuple_budget(fn)) have_native = 0;
  }
  ptx_report_tensor_matmul(fn, in, have_native, use_direct_mma, &profile,
                           &direct_profile, native_peak, native_reason);
  if (!have_native) {
    sb_printf(&fn->body,
              "\t// mtlc.tensor_matmul cooperative-only: %s\n",
              native_peak > ptx_tensor_tuple_budget(fn)
                  ? "native accumulator exceeds tensor tuple budget"
                  : native_reason);
    ptx_emit_tensor_matmul_scalar(
        fn, in, bases, spaces, strides64, scale_strides64, metadata_stride64,
        row_origin,
        column_origin,
        problem_m, problem_n, problem_k, zero64, 0, lane_rank, participants,
        mask, label_id, "full");
    sb_printf(&fn->body, "mtlc_tensor_matmul_%llu_finish:\n", label_id);
    return;
  }

  char row_remaining[24], column_remaining[24], k_remaining[24];
  char row_origin_ok[24], column_origin_ok[24];
  char row_full[24], column_full[24], k_full[24], mask_full[24];
  char native[24];
  reg_name(PC_B64, new_reg(fn, PC_B64), row_remaining);
  reg_name(PC_B64, new_reg(fn, PC_B64), column_remaining);
  reg_name(PC_B64, new_reg(fn, PC_B64), k_remaining);
  reg_name(PC_PRED, new_reg(fn, PC_PRED), row_origin_ok);
  reg_name(PC_PRED, new_reg(fn, PC_PRED), column_origin_ok);
  reg_name(PC_PRED, new_reg(fn, PC_PRED), row_full);
  reg_name(PC_PRED, new_reg(fn, PC_PRED), column_full);
  reg_name(PC_PRED, new_reg(fn, PC_PRED), k_full);
  reg_name(PC_PRED, new_reg(fn, PC_PRED), mask_full);
  reg_name(PC_PRED, new_reg(fn, PC_PRED), native);
  sb_printf(&fn->body,
            "\tsetp.le.u64 %s, %s, %s;\n"
            "\tsub.u64 %s, %s, %s;\n"
            "\tsetp.ge.u64 %s, %s, %u;\n"
            "\tand.pred %s, %s, %s;\n"
            "\tsetp.le.u64 %s, %s, %s;\n"
            "\tsub.u64 %s, %s, %s;\n"
            "\tsetp.ge.u64 %s, %s, %u;\n"
            "\tand.pred %s, %s, %s;\n"
            "\tmov.u64 %s, %s;\n"
            "\tsetp.ge.u64 %s, %s, %u;\n"
            "\tsetp.eq.u32 %s, %s, 4294967295;\n"
            "\tand.pred %s, %s, %s;\n"
            "\tand.pred %s, %s, %s;\n"
            "\tand.pred %s, %s, %s;\n",
            row_origin_ok, row_origin, problem_m, row_remaining, problem_m,
            row_origin, row_full, row_remaining, (unsigned)IR_TENSOR_MMA(in).m,
            row_full, row_origin_ok, row_full, column_origin_ok,
            column_origin, problem_n, column_remaining, problem_n,
            column_origin, column_full, column_remaining,
            (unsigned)IR_TENSOR_MMA(in).n, column_full, column_origin_ok,
            column_full, k_remaining, problem_k, k_full, k_remaining,
            (unsigned)IR_TENSOR_MMA(in).k, mask_full, mask, native, row_full,
            column_full, native, native, k_full, native, native, mask_full);
  if (sparse) {
    char metadata_stride_roundtrip[24], metadata_stride_fits[24];
    reg_name(PC_B64, new_reg(fn, PC_B64), metadata_stride_roundtrip);
    reg_name(PC_PRED, new_reg(fn, PC_PRED), metadata_stride_fits);
    sb_printf(&fn->body,
              "\tcvt.u64.u32 %s, %s;\n"
              "\tsetp.eq.u64 %s, %s, %s;\n"
              "\tand.pred %s, %s, %s;\n",
              metadata_stride_roundtrip, metadata_stride32,
              metadata_stride_fits, metadata_stride_roundtrip,
              metadata_stride64, native, native, metadata_stride_fits);
  }
  ptx_tensor_matmul_wmma_guard(fn, in, use_direct_mma, bases, strides64,
                               native);
  sb_printf(&fn->body,
            "\t@%s bra mtlc_tensor_matmul_%llu_native;\n"
            "\tbra mtlc_tensor_matmul_%llu_scalar;\n",
            native, label_id, label_id);

  sb_printf(&fn->body, "mtlc_tensor_matmul_%llu_scalar:\n", label_id);
  ptx_emit_tensor_matmul_scalar(
      fn, in, bases, spaces, strides64, scale_strides64, metadata_stride64,
      row_origin,
      column_origin, problem_m, problem_n, problem_k, zero64, 0, lane_rank,
      participants, mask, label_id, "full");

  sb_printf(&fn->body,
            "mtlc_tensor_matmul_%llu_native:\n"
            "\t// mtlc.tensor_matmul native interior runtime-K resident %s tuple_peak=%d budget=%d\n",
            label_id, use_direct_mma ? "direct-mma" : "stable-wmma",
            native_peak, ptx_tensor_tuple_budget(fn));
  char pointer_names[6][64];
  IROperand tile_arguments[11];
  if (per_tile > 11) {
    fn_error(fn, "PTX tensor_matmul internal operand bundle is too large");
    return;
  }
  memcpy(tile_arguments, in->arguments, per_tile * sizeof(*tile_arguments));
  for (size_t i = 0; i < pointer_count; i++) {
    snprintf(pointer_names[i], sizeof(pointer_names[i]),
             "$mtlc_matmul_%llu_ptr_%llu", label_id,
             (unsigned long long)i);
    tile_arguments[i] = (IROperand){0};
    tile_arguments[i].kind = IR_OPERAND_TEMP;
    tile_arguments[i].name = pointer_names[i];
  }
  IRInstruction tile = *in;
  IRTensorAux tile_tensor;
  ir_instruction_tensor_borrow(&tile, &tile_tensor, in);
  tile_tensor.mma = native_desc;
  tile.op = IR_OP_TENSOR_MMA;
  tile.arguments = tile_arguments;
  tile.argument_count = per_tile;
  tile.tensor_mma_count = 1;
  tile.tensor_residency_id = 0;
  tile.tensor_residency_role = IR_TENSOR_RESIDENCY_NONE;
  tile.tensor_residency_scope = IR_TENSOR_RESIDENCY_SCOPE_NONE;

  char initial_addresses[6][24], a_aligned[24], b_aligned[24];
  int initial_indices[6] = {-1, -1, -1, -1, -1, -1};
  initial_indices[0] = ptx_tensor_matmul_storage_address(
      fn, bases[0], row_origin, zero64, strides64[0], native_desc.a_layout,
      IR_TENSOR_MMA(in).a_element, IR_TENSOR_MMA(in).a_packing,
      initial_addresses[0], a_aligned);
  initial_indices[1] = ptx_tensor_matmul_storage_address(
      fn, bases[1], zero64, column_origin, strides64[1], native_desc.b_layout,
      IR_TENSOR_MMA(in).b_element, IR_TENSOR_MMA(in).b_packing,
      initial_addresses[1], b_aligned);
  initial_indices[2] = ptx_tensor_matmul_address(
      fn, bases[2], row_origin, column_origin, strides64[2],
      IR_TENSOR_MMA(in).c_layout,
      (unsigned)ptx_tensor_matmul_element_bytes(
          IR_TENSOR_MMA(in).accumulator_element),
      initial_addresses[2]);
  initial_indices[3] = ptx_tensor_matmul_address(
      fn, bases[3], row_origin, column_origin, strides64[3],
      IR_TENSOR_MMA(in).d_layout,
      (unsigned)ptx_tensor_matmul_element_bytes(
          IR_TENSOR_MMA(in).result_element),
      initial_addresses[3]);
  if (sparse) {
    initial_indices[4] = ptx_tensor_matmul_address(
        fn, bases[4], row_origin, zero64, metadata_stride64,
        MTLC_TENSOR_LAYOUT_ROW_MAJOR, 1, initial_addresses[4]);
  } else if (scaled) {
    initial_indices[4] = ptx_tensor_matmul_address(
        fn, bases[4], row_origin, zero64, scale_strides64[0],
        MTLC_TENSOR_LAYOUT_ROW_MAJOR, 1, initial_addresses[4]);
    initial_indices[5] = ptx_tensor_matmul_address(
        fn, bases[5], zero64, column_origin, scale_strides64[1],
        MTLC_TENSOR_LAYOUT_COLUMN_MAJOR, 1, initial_addresses[5]);
  }
  char packed_aligned[24];
  reg_name(PC_PRED, new_reg(fn, PC_PRED), packed_aligned);
  sb_printf(&fn->body, "\tand.pred %s, %s, %s;\n", packed_aligned,
            a_aligned, b_aligned);
  int force_dense_contiguous[2] = {
      use_direct_mma && direct_profile.a_bits < 8 &&
          IR_TENSOR_MMA(in).a_packing == MTLC_TENSOR_PACKING_DENSE_SUBBYTE &&
          native_desc.a_layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR &&
          !native_desc.transpose_a,
      use_direct_mma && direct_profile.b_bits < 8 &&
          IR_TENSOR_MMA(in).b_packing == MTLC_TENSOR_PACKING_DENSE_SUBBYTE &&
          native_desc.b_layout == MTLC_TENSOR_LAYOUT_COLUMN_MAJOR &&
          !native_desc.transpose_b};
  int direct_bits[2] = {use_direct_mma ? direct_profile.a_bits : 0,
                        use_direct_mma ? direct_profile.b_bits : 0};
  for (size_t operand = 0; operand < 2; operand++) {
    if (!force_dense_contiguous[operand]) continue;
    unsigned stride_mask = direct_bits[operand] == 4 ? 1u : 3u;
    char remainder[24], stride_aligned[24];
    reg_name(PC_B64, new_reg(fn, PC_B64), remainder);
    reg_name(PC_PRED, new_reg(fn, PC_PRED), stride_aligned);
    sb_printf(&fn->body,
              "\tand.b64 %s, %s, %u;\n"
              "\tsetp.eq.u64 %s, %s, 0;\n"
              "\tand.pred %s, %s, %s;\n",
              remainder, strides64[operand], stride_mask, stride_aligned,
              remainder, packed_aligned, packed_aligned, stride_aligned);
  }
  sb_printf(&fn->body,
            "\t// mtlc.tensor_matmul dense-subbyte byte-alignment guard\n"
            "\t@!%s bra mtlc_tensor_matmul_%llu_scalar;\n",
            packed_aligned, label_id);
  for (size_t i = 0; i < pointer_count; i++) {
    if (!ptx_tensor_matmul_bind_pointer(fn, pointer_names[i],
                                        pointer_descs[i],
                                        initial_indices[i])) {
      fn_error(fn, "PTX tensor_matmul could not bind a derived tile pointer");
      return;
    }
  }
  if (use_direct_mma) {
    int direct_accumulator_count =
        direct_profile.m_tiles * direct_profile.n_tiles *
        direct_profile.accumulator_registers;
    int direct_accumulator_base = fn->count[PC_F32];
    for (int i = 0; i < direct_accumulator_count; i++) new_reg(fn, PC_F32);

    char lane[24], group[24], thread[24];
    reg_name(PC_B32, new_reg(fn, PC_B32), lane);
    reg_name(PC_B32, new_reg(fn, PC_B32), group);
    reg_name(PC_B32, new_reg(fn, PC_B32), thread);
    sb_printf(&fn->body,
              "\tmov.u32 %s, %%laneid;\n"
              "\tshr.u32 %s, %s, 2;\n"
              "\tand.b32 %s, %s, 3;\n",
              lane, group, lane, thread, lane);

    PtxMmaTileMemory direct_memory;
    if (!ptx_mma_prepare_tile_memory(fn, &tile, &direct_profile, 0, per_tile,
                                     &direct_memory)) {
      fn_error(fn,
               "PTX tensor_matmul direct MMA has invalid tile pointers or strides");
      return;
    }
    for (size_t operand = 0; operand < 2; operand++)
      if (force_dense_contiguous[operand])
        direct_memory.dense_contiguous[operand] = 1;
    if (sparse)
      snprintf(direct_memory.metadata_stride,
               sizeof(direct_memory.metadata_stride), "%s",
               metadata_stride32);
    for (int m_tile = 0; m_tile < direct_profile.m_tiles; m_tile++) {
      PtxMmaSparseAFragment sparse_a;
      PtxMmaSparseAFragment *sparse_a_ptr = NULL;
      if (sparse) {
        ptx_mma_prepare_sparse_f16_a(
            fn, &tile, &direct_memory, group, thread,
            (unsigned)m_tile * 16u, &sparse_a);
        sparse_a_ptr = &sparse_a;
      }
      for (int n_tile = 0; n_tile < direct_profile.n_tiles; n_tile++) {
        int subtile = m_tile * direct_profile.n_tiles + n_tile;
        ptx_emit_mma_native_subtile(
            fn, &tile, &direct_profile, &direct_memory, group, thread,
            sparse_a_ptr, (unsigned)m_tile * 16u, (unsigned)n_tile * 8u,
            direct_accumulator_base +
                subtile * direct_profile.accumulator_registers,
            1, 0);
      }
    }
    if (fn->error) return;

    char direct_q[24], direct_remaining[24], direct_loop_done[24];
    reg_name(PC_B64, new_reg(fn, PC_B64), direct_q);
    reg_name(PC_B64, new_reg(fn, PC_B64), direct_remaining);
    reg_name(PC_PRED, new_reg(fn, PC_PRED), direct_loop_done);
    sb_printf(&fn->body,
              "\tmov.u64 %s, %u;\n"
              "mtlc_tensor_matmul_%llu_native_k:\n"
              "\tsub.u64 %s, %s, %s;\n"
              "\tsetp.lt.u64 %s, %s, %u;\n"
              "\t@%s bra mtlc_tensor_matmul_%llu_native_store;\n",
              direct_q, (unsigned)IR_TENSOR_MMA(in).k, label_id,
              direct_remaining, problem_k, direct_q, direct_loop_done,
              direct_remaining, (unsigned)IR_TENSOR_MMA(in).k,
              direct_loop_done, label_id);
    char direct_update_a[24], direct_update_b[24], direct_a_q[24];
    char direct_a_aligned[24], direct_b_aligned[24];
    const char *direct_a_column = direct_q;
    if (sparse) {
      reg_name(PC_B64, new_reg(fn, PC_B64), direct_a_q);
      sb_printf(&fn->body, "\tshr.u64 %s, %s, 1;\n", direct_a_q,
                direct_q);
      direct_a_column = direct_a_q;
    }
    int direct_update_a_index = ptx_tensor_matmul_storage_address(
        fn, bases[0], row_origin, direct_a_column, strides64[0],
        native_desc.a_layout, IR_TENSOR_MMA(in).a_element,
        IR_TENSOR_MMA(in).a_packing, direct_update_a, direct_a_aligned);
    int direct_update_b_index = ptx_tensor_matmul_storage_address(
        fn, bases[1], direct_q, column_origin, strides64[1],
        native_desc.b_layout, IR_TENSOR_MMA(in).b_element,
        IR_TENSOR_MMA(in).b_packing, direct_update_b, direct_b_aligned);
    int direct_bound =
        ptx_tensor_matmul_bind_pointer(fn, pointer_names[0], pointer_descs[0],
                                       direct_update_a_index) &&
        ptx_tensor_matmul_bind_pointer(fn, pointer_names[1], pointer_descs[1],
                                       direct_update_b_index);
    if (sparse) {
      char direct_metadata_group[24], direct_update_metadata[24];
      reg_name(PC_B64, new_reg(fn, PC_B64), direct_metadata_group);
      sb_printf(&fn->body, "\tshr.u64 %s, %s, 2;\n",
                direct_metadata_group, direct_q);
      int direct_update_metadata_index = ptx_tensor_matmul_address(
          fn, bases[4], row_origin, direct_metadata_group,
          metadata_stride64, MTLC_TENSOR_LAYOUT_ROW_MAJOR, 1,
          direct_update_metadata);
      direct_bound =
          direct_bound &&
          ptx_tensor_matmul_bind_pointer(
              fn, pointer_names[4], pointer_descs[4],
              direct_update_metadata_index);
    } else if (scaled) {
      unsigned scale_shift =
          IR_TENSOR_MMA(in).a_scale_mode == MTLC_TENSOR_SCALE_BLOCK_16 ? 4 : 5;
      char direct_scale_chunk[24], direct_update_scale_a[24];
      char direct_update_scale_b[24];
      reg_name(PC_B64, new_reg(fn, PC_B64), direct_scale_chunk);
      sb_printf(&fn->body, "\tshr.u64 %s, %s, %u;\n", direct_scale_chunk,
                direct_q, scale_shift);
      int direct_update_scale_a_index = ptx_tensor_matmul_address(
          fn, bases[4], row_origin, direct_scale_chunk, scale_strides64[0],
          MTLC_TENSOR_LAYOUT_ROW_MAJOR, 1, direct_update_scale_a);
      int direct_update_scale_b_index = ptx_tensor_matmul_address(
          fn, bases[5], direct_scale_chunk, column_origin, scale_strides64[1],
          MTLC_TENSOR_LAYOUT_COLUMN_MAJOR, 1, direct_update_scale_b);
      direct_bound =
          direct_bound &&
          ptx_tensor_matmul_bind_pointer(fn, pointer_names[4], pointer_descs[4],
                                         direct_update_scale_a_index) &&
          ptx_tensor_matmul_bind_pointer(fn, pointer_names[5], pointer_descs[5],
                                         direct_update_scale_b_index);
    }
    if (!direct_bound) {
      fn_error(fn,
               "PTX tensor_matmul could not bind direct-MMA K-chunk pointers");
      return;
    }
    PtxMmaTileMemory direct_update_memory;
    if (!ptx_mma_prepare_tile_memory(fn, &tile, &direct_profile, 0, per_tile,
                                     &direct_update_memory)) {
      fn_error(fn,
               "PTX tensor_matmul direct MMA has invalid updated tile pointers");
      return;
    }
    for (size_t operand = 0; operand < 2; operand++)
      if (force_dense_contiguous[operand])
        direct_update_memory.dense_contiguous[operand] = 1;
    if (sparse)
      snprintf(direct_update_memory.metadata_stride,
               sizeof(direct_update_memory.metadata_stride), "%s",
               metadata_stride32);
    for (int m_tile = 0; m_tile < direct_profile.m_tiles; m_tile++) {
      PtxMmaSparseAFragment sparse_a;
      PtxMmaSparseAFragment *sparse_a_ptr = NULL;
      if (sparse) {
        ptx_mma_prepare_sparse_f16_a(
            fn, &tile, &direct_update_memory, group, thread,
            (unsigned)m_tile * 16u, &sparse_a);
        sparse_a_ptr = &sparse_a;
      }
      for (int n_tile = 0; n_tile < direct_profile.n_tiles; n_tile++) {
        int subtile = m_tile * direct_profile.n_tiles + n_tile;
        ptx_emit_mma_native_subtile(
            fn, &tile, &direct_profile, &direct_update_memory, group, thread,
            sparse_a_ptr, (unsigned)m_tile * 16u, (unsigned)n_tile * 8u,
            direct_accumulator_base +
                subtile * direct_profile.accumulator_registers,
            0, 0);
      }
    }
    if (fn->error) return;
    sb_printf(&fn->body,
              "\tadd.u64 %s, %s, %u;\n"
              "\tbra mtlc_tensor_matmul_%llu_native_k;\n"
              "mtlc_tensor_matmul_%llu_native_store:\n",
              direct_q, direct_q, (unsigned)IR_TENSOR_MMA(in).k, label_id,
              label_id);
    for (int m_tile = 0; m_tile < direct_profile.m_tiles; m_tile++) {
      for (int n_tile = 0; n_tile < direct_profile.n_tiles; n_tile++) {
        int subtile = m_tile * direct_profile.n_tiles + n_tile;
        ptx_mma_store_f32_accumulator_subtile(
            fn, &tile, &direct_profile, direct_memory.bases[3],
            direct_memory.spaces[3], direct_memory.strides[3], group, thread,
            (unsigned)m_tile * 16u, (unsigned)n_tile * 8u,
            direct_accumulator_base +
                subtile * direct_profile.accumulator_registers);
      }
    }
    sb_printf(&fn->body, "\tbar.warp.sync %s;\n", mask);
    char direct_has_tail[24];
    reg_name(PC_PRED, new_reg(fn, PC_PRED), direct_has_tail);
    sb_printf(&fn->body,
              "\tsetp.lt.u64 %s, %s, %s;\n"
              "\t@%s bra mtlc_tensor_matmul_%llu_tail;\n"
              "\tbra mtlc_tensor_matmul_%llu_finish;\n"
              "mtlc_tensor_matmul_%llu_tail:\n",
              direct_has_tail, direct_q, problem_k, direct_has_tail, label_id,
              label_id, label_id);
    ptx_emit_tensor_matmul_scalar(
        fn, in, bases, spaces, strides64, scale_strides64, metadata_stride64,
        row_origin,
        column_origin, problem_m, problem_n, problem_k, direct_q, 1, lane_rank,
        participants, mask, label_id, "tail");
    sb_printf(&fn->body, "mtlc_tensor_matmul_%llu_finish:\n", label_id);
    return;
  }
  int accumulator_count =
      profile.m_tiles * profile.n_tiles * profile.d_registers;
  int accumulator_base = fn->count[profile.d_class];
  for (int i = 0; i < accumulator_count; i++) new_reg(fn, profile.d_class);
  if (!ptx_emit_wmma_tiled_tile(fn, &tile, &profile, 0, per_tile,
                                 accumulator_base, 1, 0))
    return;

  char q[24], remaining[24], loop_done[24];
  reg_name(PC_B64, new_reg(fn, PC_B64), q);
  reg_name(PC_B64, new_reg(fn, PC_B64), remaining);
  reg_name(PC_PRED, new_reg(fn, PC_PRED), loop_done);
  sb_printf(&fn->body,
            "\tmov.u64 %s, %u;\n"
            "mtlc_tensor_matmul_%llu_native_k:\n"
            "\tsub.u64 %s, %s, %s;\n"
            "\tsetp.lt.u64 %s, %s, %u;\n"
            "\t@%s bra mtlc_tensor_matmul_%llu_native_store;\n",
            q, (unsigned)IR_TENSOR_MMA(in).k, label_id, remaining, problem_k, q,
            loop_done, remaining, (unsigned)IR_TENSOR_MMA(in).k, loop_done,
            label_id);
  char update_a[24], update_b[24];
  int update_a_index = ptx_tensor_matmul_address(
      fn, bases[0], row_origin, q, strides64[0], native_desc.a_layout,
      (unsigned)ptx_tensor_matmul_element_bytes(IR_TENSOR_MMA(in).a_element),
      update_a);
  int update_b_index = ptx_tensor_matmul_address(
      fn, bases[1], q, column_origin, strides64[1],
      native_desc.b_layout,
      (unsigned)ptx_tensor_matmul_element_bytes(IR_TENSOR_MMA(in).b_element),
      update_b);
  if (!ptx_tensor_matmul_bind_pointer(fn, pointer_names[0], pointer_descs[0],
                                      update_a_index) ||
      !ptx_tensor_matmul_bind_pointer(fn, pointer_names[1], pointer_descs[1],
                                      update_b_index) ||
      !ptx_emit_wmma_tiled_tile(fn, &tile, &profile, 0, per_tile,
                                accumulator_base, 0, 0))
    return;
  sb_printf(&fn->body,
            "\tadd.u64 %s, %s, %u;\n"
            "\tbra mtlc_tensor_matmul_%llu_native_k;\n"
            "mtlc_tensor_matmul_%llu_native_store:\n",
            q, q, (unsigned)IR_TENSOR_MMA(in).k, label_id, label_id);
  if (!ptx_emit_wmma_tiled_store(fn, &tile, &profile, 0, per_tile,
                                  accumulator_base))
    return;
  sb_printf(&fn->body, "\tbar.warp.sync %s;\n", mask);
  char has_tail[24];
  reg_name(PC_PRED, new_reg(fn, PC_PRED), has_tail);
  sb_printf(&fn->body,
            "\tsetp.lt.u64 %s, %s, %s;\n"
            "\t@%s bra mtlc_tensor_matmul_%llu_tail;\n"
            "\tbra mtlc_tensor_matmul_%llu_finish;\n"
            "mtlc_tensor_matmul_%llu_tail:\n",
            has_tail, q, problem_k, has_tail, label_id, label_id, label_id);
  ptx_emit_tensor_matmul_scalar(
      fn, in, bases, spaces, strides64, scale_strides64, metadata_stride64,
      row_origin,
      column_origin, problem_m, problem_n, problem_k, q, 1, lane_rank,
      participants, mask, label_id, "tail");
  sb_printf(&fn->body, "mtlc_tensor_matmul_%llu_finish:\n", label_id);
}

static void emit_binary(PtxFn *fn, const IRInstruction *in);
static void emit_unary(PtxFn *fn, const IRInstruction *in);
static void emit_function(IRProgram *program, size_t fi, CodeGenerator *gen,
                          FILE *out, int target_arch, char target_variant,
                          int isa_major, int isa_minor,
                          int tensor_tuple_budget, char **error);

static int ptx_target_valid(const char *target) {
  if (!target) {
    return 0;
  }
  int is_sm = strncmp(target, "sm_", 3) == 0;
  const unsigned char *p = (const unsigned char *)target;
  if (is_sm) {
    p += 3;
  } else if (strncmp(target, "compute_", 8) == 0) {
    p += 8;
  } else {
    return 0;
  }
  if (!isdigit(*p)) {
    return 0;
  }
  while (isdigit(*p)) {
    p++;
  }
  if (is_sm && (*p == 'a' || *p == 'f')) {
    p++;
  }
  return *p == '\0';
}

static int ptx_target_arch(const char *target) {
  const char *digits;
  if (!ptx_target_valid(target)) return 0;
  digits = strncmp(target, "sm_", 3) == 0 ? target + 3 : target + 8;
  return atoi(digits);
}

static char ptx_target_variant(const char *target) {
  const char *digits;
  const char *tail;
  if (!ptx_target_valid(target)) return '\0';
  digits = strncmp(target, "sm_", 3) == 0 ? target + 3 : target + 8;
  tail = digits;
  while (isdigit((unsigned char)*tail)) tail++;
  return (*tail == 'a' || *tail == 'f') ? *tail : '\0';
}

int ptx_emit_program(IRProgram *program, CodeGenerator *generator, FILE *out,
                     const PtxEmitOptions *options, char **error) {
  if (error) {
    *error = NULL;
  }
  if (!program || !out) {
    if (error) {
      *error = strdup("ptx_emit_program: null program/out");
    }
    return 0;
  }
  const char *target = options && options->target ? options->target : "sm_121a";
  int isa_major = options ? options->isa_major : 8;
  int isa_minor = options ? options->isa_minor : 8;
  int tensor_tuple_budget = options ? options->tensor_tuple_budget : 0;
  g_ptx_emit_checks = options ? options->checks : 0;
  g_ptx_report_types = options ? options->report_types : 0;
  g_ptx_spaced_accesses = 0;
  g_ptx_generic_accesses = 0;
  g_ptx_vector_groups = 0;
  g_ptx_vector_loads_saved = 0;
  g_ptx_analysis_seconds = 0.0;
  if (!ptx_target_valid(target) || isa_major < 1 || isa_major > 99 ||
      isa_minor < 0 || isa_minor > 9 || tensor_tuple_budget < 0 ||
      tensor_tuple_budget > 4096) {
    if (error) {
      *error = strdup("PTX target options are invalid");
    }
    return 0;
  }
  char *asm_error = NULL;
  if (!ir_program_bind_device_asm(program, &asm_error)) {
    if (error) *error = asm_error;
    else free(asm_error);
    return 0;
  }
  char profile[64];
  snprintf(profile, sizeof(profile), "%d.%d for %s", isa_major, isa_minor,
           target);
  ir_explain_device_begin("ptx", profile);
  IRGpuCallGraph graph = {0};
  char *graph_error = NULL;
  if (!ir_program_build_gpu_call_graph(program, &graph, &graph_error)) {
    if (error) *error = graph_error ? graph_error
                                    : strdup("PTX: invalid GPU call graph");
    else free(graph_error);
    return 0;
  }
  fprintf(out, "//\n// Generated by the Mettle PTX backend (--emit-ptx).\n//\n");
  fprintf(out, ".version %d.%d\n.target %s\n.address_size 64\n\n",
          isa_major, isa_minor, target);

  ptx_string_pool_reset();
  for (size_t oi = 0; oi < graph.count; oi++) {
    IRFunction *function = program->functions[graph.order[oi]];
    if (!function) continue;
    for (size_t ii = 0; ii < function->instruction_count; ii++) {
      const IRInstruction *in = &function->instructions[ii];
      const char *format = NULL;
      if (in->op != IR_OP_CALL || !ptx_intrinsic_is_print(in->intrinsic) ||
          in->argument_count < 1) {
        continue;
      }
      format = ptx_literal_string(function, &in->arguments[0]);
      if (!format) continue;
      g_ptx_uses_vprintf = 1;
      if (ptx_string_intern(format) < 0) {
        if (error) *error = strdup("PTX: out of memory interning a format string");
        ir_gpu_call_graph_destroy(&graph);
        ptx_string_pool_reset();
        return 0;
      }
    }
  }
  if (g_ptx_uses_vprintf) {
    fprintf(out, ".extern .func (.param .b32 func_retval0) vprintf\n"
                 "(\n\t.param .b64 vprintf_format,\n"
                 "\t.param .b64 vprintf_arguments\n);\n");
    for (size_t i = 0; i < g_ptx_string_count; i++) {
      const char *text = g_ptx_strings[i];
      size_t length = strlen(text);
      fprintf(out, ".global .align 1 .b8 $mtlc_str_%zu[%zu] = {", i,
              length + 1);
      for (size_t c = 0; c < length; c++) {
        fprintf(out, "%s%u", c ? ", " : "", (unsigned)(unsigned char)text[c]);
      }
      fprintf(out, "%s0};\n", length ? ", " : "");
    }
    fputc('\n', out);
  }
  for (size_t oi = 0; oi < graph.count; oi++) {
    IRFunction *function = program->functions[graph.order[oi]];
    size_t alignment = 0;
    if (!function || !function->is_kernel) continue;
    for (size_t ii = 0; ii < function->instruction_count; ii++) {
      const IRInstruction *in = &function->instructions[ii];
      if (in->op != IR_OP_ADDRESS_SPACE_ALLOC ||
          in->rhs.kind != IR_OPERAND_INT || in->rhs.int_value != 0 ||
          !in->value_type || in->value_type->kind != MTLC_TYPE_POINTER ||
          !in->value_type->base_type) {
        continue;
      }
      // A declared align(N) on the workgroup pointer raises the storage's
      // alignment: tensor-map tiles and their swizzles want up to 1024.
      size_t candidate = mtlc_type_alignment(in->value_type->base_type);
      if (in->value_type->pointee_align > candidate)
        candidate = in->value_type->pointee_align;
      if (in->lhs.kind == IR_OPERAND_INT && in->lhs.int_value > 0 &&
          (size_t)in->lhs.int_value > candidate)
        candidate = (size_t)in->lhs.int_value;
      if (candidate < 32) candidate = 32;
      if (candidate > alignment) alignment = candidate;
    }
    if (alignment) {
      char kernel_name[256], raw[512], storage[512];
      sanitize_into(function->name ? function->name : "kernel", kernel_name,
                    sizeof(kernel_name));
      snprintf(raw, sizeof(raw), "%s_dynamic_workgroup_storage", kernel_name);
      sanitize_into(raw, storage, sizeof(storage));
      fprintf(out, ".extern .shared .align %zu .b8 %s[];\n", alignment,
              storage);
    }
  }
  fputc('\n', out);
  for (size_t oi = 0; oi < graph.count; oi++) {
    size_t i = graph.order[oi];
    char *ferr = NULL;
    emit_function(program, i, generator, out, ptx_target_arch(target),
                  ptx_target_variant(target), isa_major, isa_minor,
                  tensor_tuple_budget, &ferr);
    if (ferr) {
      if (error) {
        *error = ferr;
      } else {
        free(ferr);
      }
      ir_gpu_call_graph_destroy(&graph);
      return 0;
    }
  }
  ir_gpu_call_graph_destroy(&graph);
  if (g_ptx_report_types) {
    long long total = g_ptx_spaced_accesses + g_ptx_generic_accesses;
    fprintf(stderr,
            "GPU type report: %lld of %lld device accesses named their space, "
            "%lld stayed generic\n",
            g_ptx_spaced_accesses, total, g_ptx_generic_accesses);
    fprintf(stderr,
            "  %lld vector groups formed from a declared alignment, "
            "%lld separate loads removed\n",
            g_ptx_vector_groups, g_ptx_vector_loads_saved);
    fprintf(stderr, "  the analyses took %.3f ms\n",
            g_ptx_analysis_seconds * 1000.0);
  }
  return 1;
}

static const char *param_storage_type(PtxVal value) {
  if (value.is_ptr) {
    return "u64";
  }
  switch (value.elem) {
  case MTLC_TYPE_INT8:
    return "s8";
  case MTLC_TYPE_UINT8:
  case MTLC_TYPE_BOOL:
    return "u8";
  case MTLC_TYPE_INT16:
    return "s16";
  case MTLC_TYPE_UINT16:
    return "u16";
  case MTLC_TYPE_INT32:
    return "s32";
  case MTLC_TYPE_UINT32:
    return "u32";
  case MTLC_TYPE_INT64:
    return "s64";
  case MTLC_TYPE_UINT64:
    return "u64";
  case MTLC_TYPE_FLOAT32:
    return "f32";
  case MTLC_TYPE_FLOAT64:
    return "f64";
  case MTLC_TYPE_FLOAT16:
  case MTLC_TYPE_BFLOAT16:
    return "b16";
  default:
    return "u32";
  }
}

static const char *device_param_storage_type(PtxVal value) {
  switch (value.cls) {
  case PC_PRED:
  case PC_B16:
  case PC_B32:
    return "b32";
  case PC_B64:
    return "b64";
  case PC_F32:
    return "f32";
  case PC_F64:
    return "f64";
  default:
    return "b32";
  }
}

static IRFunction *ptx_lookup_function(IRProgram *program, const char *name) {
  if (!program || !name) {
    return NULL;
  }
  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *function = program->functions[i];
    if (function && function->name && strcmp(function->name, name) == 0) {
      return function;
    }
  }
  return NULL;
}

static const MtlcType *ptx_function_return_type(
    const IRProgram *program, const IRFunction *function,
    const IRModuleSymbol *symbol) {
  if (symbol && symbol->kind == IR_MODSYM_FUNCTION && symbol->return_type) {
    return symbol->return_type;
  }
  return function && function->return_type_name
             ? ir_program_lookup_type(program, function->return_type_name)
             : NULL;
}

static int ptx_type_is_void(const MtlcType *type, const char *fallback_name) {
  return (type && type->kind == MTLC_TYPE_VOID) ||
         (!type && fallback_name && strcmp(fallback_name, "void") == 0);
}

#include "ptx_emitter_tile.inc"

static void ptx_emit_device(IRProgram *program, IRFunction *func, PtxFn *fn,
                        const IRInstruction *in, size_t *ii, char **error,
                        int target_arch, int returns_void, const char *ename,
                        int *handled) {
  (void)ename;
  (void)returns_void;
  (void)target_arch;
  (void)program;
  (void)error;
  switch (in->op) {
  case IR_OP_NOP:
  case IR_OP_ADDRESS_SPACE_ALLOC:
  case IR_OP_DECLARE_LOCAL: {
    if (in->op == IR_OP_DECLARE_LOCAL && in->dest.name) {
      PtxVal d = in->value_type ? descriptor_from_type(in->value_type)
                                : descriptor_from_typename(in->text);
      if (d.cls == PC_NONE) {
        fn_error(fn, "PTX: local '%s' has unsupported type '%s'",
                 in->dest.name, in->text ? in->text : "?");
        break;
      }
      if (d.is_ptr && ptx_space_is_plain(in->value_type, in->text) &&
          ptx_name_may_be_generic(fn, in->dest.name)) {
        d.address_space = MTLC_ADDRESS_SPACE_GENERIC;
      }
      if (!find_binding(fn, in->dest.name)) {
        d.idx = new_reg(fn, d.cls);
        bind_value(fn, in->dest.name, d);
      }
    }
    break;
  }
  case IR_OP_BARRIER:
    if (!ptx_workgroup_barrier_contract(in)) {
      fn_error(fn, "PTX: invalid workgroup barrier memory contract");
    } else if (in->memory_scope == MTLC_MEMORY_SCOPE_SUBGROUP) {
      // The whole warp; bar.warp.sync also orders the participating
      // threads' memory accesses.
      sb_puts(&fn->body, "\tbar.warp.sync -1;\n");
    } else {
      sb_puts(&fn->body, "\tbar.sync 0;\n");
    }
    break;
  case IR_OP_ASYNC_COPY:
    ptx_emit_async_copy(fn, in);
    break;
  case IR_OP_ASYNC_COMMIT:
    ptx_emit_async_commit(fn);
    break;
  case IR_OP_ASYNC_WAIT:
    ptx_emit_async_wait(fn, in);
    break;
  case IR_OP_TENSOR_TRANSFER:
    ptx_emit_tensor_transfer(fn, in);
    break;
  case IR_OP_TENSOR_MMA: {
    size_t epilogue_index = 0;
    const IRInstruction *epilogue =
        ptx_following_tensor_epilogue(func, (*ii), &epilogue_index);
    if (epilogue &&
        ptx_try_emit_tensor_mma_resident_epilogue(fn, in, epilogue)) {
      (*ii) = epilogue_index;
    } else {
      ptx_emit_tensor_mma(fn, in);
    }
    break;
  }
  case IR_OP_TENSOR_MATMUL:
    ptx_emit_tensor_matmul(fn, in);
    break;
  case IR_OP_TILE:
    ptx_emit_tile(fn, in);
    break;
  case IR_OP_TENSOR_EPILOGUE:
    if (!ptx_tensor_epilogue_was_consumed(fn, in))
      ptx_emit_tensor_epilogue(fn, in);
    break;
  case IR_OP_TENSOR_SCALE_ROWS:
    fn_error(fn, "tensor_scale_rows is Metal only for now: PTX keeps a "
                 "resident accumulator in WMMA fragments, whose element "
                 "layout is not specified; use tensor_mma with "
                 "c_scale_mode: per_row on PTX");
    break;
  case IR_OP_TENSOR_COMMIT: {
    size_t epilogue_index = 0;
    const IRInstruction *epilogue =
        ptx_following_tensor_epilogue(func, (*ii), &epilogue_index);
    int deferred_loop_exit = 0;
    if (!epilogue) {
      epilogue =
          ptx_loop_exit_tensor_epilogue(func, (*ii), &epilogue_index);
      deferred_loop_exit = epilogue != NULL;
    }
    if (epilogue &&
        ptx_try_emit_tensor_commit_resident_epilogue(fn, in, epilogue)) {
      if (deferred_loop_exit) {
        PtxTensorResidency *group =
            ptx_tensor_residency_find(fn, in->tensor_residency_id);
        if (!group) {
          fn_error(fn,
                   "PTX resident loop epilogue handoff lost residency group");
        } else {
          group->consumed_epilogue = epilogue;
        }
      } else {
        (*ii) = epilogue_index;
      }
    } else {
      ptx_emit_tensor_residency_commit(fn, in);
    }
    break;
  }
  default:
    *handled = 0;
    break;
  }
}

static void ptx_emit_control(IRProgram *program, IRFunction *func, PtxFn *fn,
                        const IRInstruction *in, size_t *ii, char **error,
                        int target_arch, int returns_void, const char *ename,
                        int *handled) {
  (void)ii;
  (void)ename;
  (void)returns_void;
  (void)func;
  (void)program;
  (void)error;
  switch (in->op) {
  case IR_OP_LABEL: {
    char lbl[256];
    sanitize_into(in->text ? in->text : "L", lbl, sizeof(lbl));
    sb_printf(&fn->body, "%s:\n", lbl);
    break;
  }
  case IR_OP_JUMP: {
    char lbl[256];
    sanitize_into(in->text ? in->text : "L", lbl, sizeof(lbl));
    sb_printf(&fn->body, "\tbra %s;\n", lbl);
    break;
  }
  case IR_OP_BRANCH_ZERO: {
    size_t guard_end = 0, guard_jump = 0;
    int guarded = !in->uniform_branch && !in->uniform_condition &&
                  ptx_branch_skips_only_async_copies(func, *ii, &guard_end,
                                                     &guard_jump);
    char lbl[256], r[24];
    sanitize_into(in->text ? in->text : "L", lbl, sizeof(lbl));
    PtxVal cv = operand_desc(fn, &in->lhs);
    int p = new_reg(fn, PC_PRED);
    char pn[24];
    reg_name(PC_PRED, p, pn);
    if (cv.cls == PC_F32 || cv.cls == PC_F64) {
      use_as(fn, &in->lhs, cv.cls, r);
      if (cv.cls == PC_F32) {
        sb_printf(&fn->body, "\tsetp.eq.f32 %s, %s, 0f%08X;\n", pn, r, 0u);
      } else {
        sb_printf(&fn->body, "\tsetp.eq.f64 %s, %s, 0d%016llX;\n", pn, r, 0ull);
      }
    } else {
      PtxClass c = (cv.cls == PC_B64) ? PC_B64 : PC_B32;
      use_as(fn, &in->lhs, c, r);
      sb_printf(&fn->body, "\tsetp.eq.%s %s, %s, 0;\n",
                c == PC_B64 ? "s64" : "s32", pn, r);
    }
    if (guarded) {
      snprintf(fn->async_guard, sizeof(fn->async_guard), "!%s", pn);
      fn->async_guard_end = guard_end;
      fn->async_guard_jump = guard_jump;
      break;
    }
    sb_printf(&fn->body, "\t@%s bra%s %s;\n", pn,
              (in->uniform_branch && target_arch >= 75) ? ".uni" : "",
              lbl);
    break;
  }
  case IR_OP_BRANCH_EQ: {
    char lbl[256], a[24], bb[24];
    sanitize_into(in->text ? in->text : "L", lbl, sizeof(lbl));
    PtxVal la = operand_desc(fn, &in->lhs);
    PtxVal lb = operand_desc(fn, &in->rhs);
    PtxClass c = PC_B32;
    if (la.cls == PC_B64 || lb.cls == PC_B64) {
      c = PC_B64;
    }
    if (la.cls == PC_F32 || lb.cls == PC_F32) {
      c = PC_F32;
    }
    if (la.cls == PC_F64 || lb.cls == PC_F64) {
      c = PC_F64;
    }
    use_as(fn, &in->lhs, c, a);
    use_as(fn, &in->rhs, c, bb);
    int p = new_reg(fn, PC_PRED);
    char pn[24];
    reg_name(PC_PRED, p, pn);
    sb_printf(&fn->body, "\tsetp.eq.%s %s, %s, %s;\n",
              type_suffix_for_class(c, 0), pn, a, bb);
    sb_printf(&fn->body, "\t@%s bra%s %s;\n", pn,
              (in->uniform_branch && target_arch >= 75) ? ".uni" : "",
              lbl);
    break;
  }
  default:
    *handled = 0;
    break;
  }
}

static void ptx_emit_assign(IRProgram *program, IRFunction *func, PtxFn *fn,
                        const IRInstruction *in, size_t *ii, char **error,
                        int target_arch, int returns_void, const char *ename,
                        int *handled) {
  (void)ii;
  (void)ename;
  (void)returns_void;
  (void)target_arch;
  (void)func;
  (void)program;
  (void)error;
  switch (in->op) {
  case IR_OP_ASSIGN: {
    if (!in->dest.name) {
      fn_error(fn, "PTX: assign with no dest");
      break;
    }
    PtxBinding *db = find_binding(fn, in->dest.name);
    PtxBinding *agg = named_binding(fn, &in->lhs);
    if (agg && agg->val.mem_aggregate) {
      if (!db) {
        bind_value(fn, in->dest.name, agg->val);
        break;
      }
      if (!db->val.mem_aggregate) {
        fn_error(fn, "PTX: cannot assign a record to the scalar '%s'",
                 in->dest.name);
        break;
      }
      size_t size = db->val.mem_size < agg->val.mem_size ? db->val.mem_size
                                                         : agg->val.mem_size;
      char dst[24], src[24];
      reg_name(PC_B64, db->val.mem_addr, dst);
      reg_name(PC_B64, agg->val.mem_addr, src);
      ptx_block_copy(fn, ".local", dst, ".local", src, size,
                     db->val.mem_align);
      break;
    }
    PtxClass dc;
    if (db) {
      dc = db->val.cls;
    } else {
      PtxVal sv = operand_desc(fn, &in->lhs);
      dc = (sv.cls == PC_NONE || sv.cls == PC_PRED) ? PC_B32 : sv.cls;
    }
    char src[24];
    use_as(fn, &in->lhs, dc, src);
    PtxVal dv;
    if (db && db->val.mem_local) {
      dv = (PtxVal){0};
      dv.cls = dc;
      dv.is_unsigned = db->val.is_unsigned;
      dv.idx = new_reg(fn, dc);
    } else if (db) {
      dv = db->val;
    } else {
      dv = operand_desc(fn, &in->lhs);
      dv.cls = dc;
      dv.idx = new_reg(fn, dc);
    }
    char dn[24];
    reg_name(dv.cls, dv.idx, dn);
    PtxVal sv = operand_desc(fn, &in->lhs);
    int converted = dv.is_ptr && sv.is_ptr &&
                    ptx_convert_space_into(fn, sv.address_space,
                                           dv.address_space, dn, src);
    if (!converted && strcmp(dn, src) != 0) {
      const char *mt = (dc == PC_F32)   ? "f32"
                       : (dc == PC_F64) ? "f64"
                       : (dc == PC_B64) ? "u64"
                                        : "u32";
      sb_printf(&fn->body, "\tmov.%s %s, %s;\n", mt, dn, src);
    }
    bind_value(fn, in->dest.name, dv);
    break;
  }
  default:
    *handled = 0;
    break;
  }
}

static void ptx_report_access(PtxFn *fn, const IRFunction *func, size_t index,
                              const IROperand *address, int generic) {
  const char *root = NULL;
  char shown[128], subject[176];
  if (!fn->reported) return;
  ir_explain_device_add(func->name,
                        generic ? IR_DEVICE_ACCESSES_GENERIC
                                : IR_DEVICE_ACCESSES_NAMED,
                        1);
  if (!generic) return;
  root = ir_device_report_address_root(func, index, address);
  if (root) {
    ir_display_name(root, shown, sizeof(shown));
    snprintf(subject, sizeof(subject), "accesses through `%s`", shown);
  } else {
    snprintf(subject, sizeof(subject), "accesses through a computed pointer");
  }
  ir_explain_device_note_site(
      func->name, func->instructions[index].location.line, "access", subject,
      IR_DEVICE_MISS, "generic address space",
      "nothing names the space this pointer points into, so each access "
      "resolves it at run time",
      "declare the space in the pointer's type: `T global*`, `T shared*` or "
      "`T local*`",
      "generic-access");
}

static void ptx_emit_memory(IRProgram *program, IRFunction *func, PtxFn *fn,
                        const IRInstruction *in, size_t *ii, char **error,
                        int target_arch, int returns_void, const char *ename,
                        int *handled) {
  (void)ename;
  (void)returns_void;
  (void)target_arch;
  (void)program;
  (void)error;
  switch (in->op) {
  case IR_OP_LOAD: {
    if (in->lhs.kind == IR_OPERAND_STRING) {
      break;
    }
    PtxVal addr = operand_desc(fn, &in->lhs);
    MtlcTypeKind elem = addr.is_ptr ? addr.elem : MTLC_TYPE_VOID;
    if (elem == MTLC_TYPE_VOID) {
      long long sz = (in->rhs.kind == IR_OPERAND_INT) ? in->rhs.int_value : 4;
      if (in->is_float) {
        if (sz == 2 && in->alias_class == IR_ALIAS_CLASS_F16) {
          elem = MTLC_TYPE_FLOAT16;
        } else if (sz == 2 && in->alias_class == IR_ALIAS_CLASS_BF16) {
          elem = MTLC_TYPE_BFLOAT16;
        } else {
          elem = (sz == 4) ? MTLC_TYPE_FLOAT32 : MTLC_TYPE_FLOAT64;
        }
      } else {
        elem = (sz == 8) ? MTLC_TYPE_INT64
               : (sz == 2) ? MTLC_TYPE_INT16
               : (sz == 1) ? MTLC_TYPE_UINT8
                           : MTLC_TYPE_INT32;
      }
    }
    char addrreg[24];
    use_as(fn, &in->lhs, PC_B64, addrreg);
    int u = 0;
    PtxClass dc = elem_class(elem, &u);
    PtxVal dv = {0};
    dv.cls = dc;
    dv.is_unsigned = u;
    dv = destination_value(fn, &in->dest, dv);
    char dn[24];
    reg_name(dc, dv.idx, dn);
    const char *space = ptx_load_space(addr.address_space);
    if (addr.address_space == MTLC_ADDRESS_SPACE_DEFAULT ||
        addr.address_space == MTLC_ADDRESS_SPACE_GENERIC) {
      g_ptx_generic_accesses++;
      ptx_report_access(fn, func, *ii, &in->lhs, 1);
    } else {
      g_ptx_spaced_accesses++;
      ptx_report_access(fn, func, *ii, &in->lhs, 0);
    }
    if (!space) {
      fn_error(fn, "PTX: invalid load address space %d",
               (int)addr.address_space);
    } else if (elem == MTLC_TYPE_FLOAT16 || elem == MTLC_TYPE_BFLOAT16) {
      char tmp[24];
      reg_name(PC_B16, new_reg(fn, PC_B16), tmp);
      sb_printf(&fn->body, "\tld%s.b16 %s, [%s];\n", space, tmp, addrreg);
      sb_printf(&fn->body, "\tcvt.f32.%s %s, %s;\n",
                elem == MTLC_TYPE_FLOAT16 ? "f16" : "bf16", dn, tmp);
    } else {
      sb_printf(&fn->body, "\tld%s.%s %s, [%s];\n", space,
                mem_type_suffix(elem), dn, addrreg);
    }
    if (in->dest.name) {
      bind_value(fn, in->dest.name, dv);
    }
    break;
  }
  case IR_OP_STORE: {
    PtxVal addr = operand_desc(fn, &in->dest);
    if (in->rhs.kind == IR_OPERAND_INT && in->rhs.int_value > 8) {
      long long total = in->rhs.int_value;
      long long offset = 0;
      PtxVal source = operand_desc(fn, &in->lhs);
      const char *dst_space = ptx_memory_space(addr.address_space);
      const char *src_space =
          source.is_ptr ? ptx_load_space(source.address_space) : "";
      char dstreg[24], srcreg[24];
      use_as(fn, &in->dest, PC_B64, dstreg);
      use_as(fn, &in->lhs, PC_B64, srcreg);
      if (!src_space) {
        src_space = "";
      }
      if (addr.address_space == MTLC_ADDRESS_SPACE_CONSTANT) {
        fn_error(fn, "PTX: store to constant address space");
        break;
      }
      if (!dst_space) {
        fn_error(fn, "PTX: invalid store address space %d",
                 (int)addr.address_space);
        break;
      }
      long long widest = ptx_copy_alignment(&addr, &source);
      while (offset < total) {
        long long width = widest;
        while (width > total - offset) {
          width >>= 1;
        }
        const char *suffix = width == 8 ? "b64"
                             : width == 4 ? "b32"
                             : width == 2 ? "u16" : "u8";
        PtxClass cls = width == 8 ? PC_B64 : PC_B32;
        char tmp[24];
        reg_name(cls, new_reg(fn, cls), tmp);
        sb_printf(&fn->body, "\tld%s.%s %s, [%s+%lld];\n", src_space,
                  suffix, tmp, srcreg, offset);
        sb_printf(&fn->body, "\tst%s.%s [%s+%lld], %s;\n", dst_space,
                  suffix, dstreg, offset, tmp);
        offset += width;
      }
      break;
    }
    MtlcTypeKind elem = addr.is_ptr ? addr.elem : MTLC_TYPE_VOID;
    if (elem == MTLC_TYPE_VOID) {
      long long sz = (in->rhs.kind == IR_OPERAND_INT) ? in->rhs.int_value : 4;
      if (in->is_float) {
        if (sz == 2 && in->alias_class == IR_ALIAS_CLASS_F16) {
          elem = MTLC_TYPE_FLOAT16;
        } else if (sz == 2 && in->alias_class == IR_ALIAS_CLASS_BF16) {
          elem = MTLC_TYPE_BFLOAT16;
        } else {
          elem = (sz == 4) ? MTLC_TYPE_FLOAT32 : MTLC_TYPE_FLOAT64;
        }
      } else {
        elem = (sz == 8) ? MTLC_TYPE_INT64
               : (sz == 2) ? MTLC_TYPE_INT16
               : (sz == 1) ? MTLC_TYPE_UINT8
                           : MTLC_TYPE_INT32;
      }
    }
    int u = 0;
    PtxClass vc = elem_class(elem, &u);
    char addrreg[24], valreg[24];
    use_as(fn, &in->dest, PC_B64, addrreg);
    use_as(fn, &in->lhs, vc, valreg);
    if (vc == PC_B64) {
      ptx_generic_address(fn, &in->lhs, valreg);
    }
    const char *space = ptx_memory_space(addr.address_space);
    if (addr.address_space == MTLC_ADDRESS_SPACE_DEFAULT ||
        addr.address_space == MTLC_ADDRESS_SPACE_GENERIC) {
      g_ptx_generic_accesses++;
      ptx_report_access(fn, func, *ii, &in->dest, 1);
    } else {
      g_ptx_spaced_accesses++;
      ptx_report_access(fn, func, *ii, &in->dest, 0);
    }
    if (addr.address_space == MTLC_ADDRESS_SPACE_CONSTANT) {
      fn_error(fn, "PTX: store to constant address space");
    } else if (!space) {
      fn_error(fn, "PTX: invalid store address space %d",
               (int)addr.address_space);
    } else if (elem == MTLC_TYPE_FLOAT16 || elem == MTLC_TYPE_BFLOAT16) {
      char tmp[24];
      reg_name(PC_B16, new_reg(fn, PC_B16), tmp);
      sb_printf(&fn->body, "\tcvt.rn.%s.f32 %s, %s;\n",
                elem == MTLC_TYPE_FLOAT16 ? "f16" : "bf16", tmp, valreg);
      sb_printf(&fn->body, "\tst%s.b16 [%s], %s;\n", space, addrreg, tmp);
    } else {
      sb_printf(&fn->body, "\tst%s.%s [%s], %s;\n", space,
                mem_type_suffix(elem), addrreg, valreg);
    }
    break;
  }
  default:
    *handled = 0;
    break;
  }
}

static void ptx_emit_arith(IRProgram *program, IRFunction *func, PtxFn *fn,
                        const IRInstruction *in, size_t *ii, char **error,
                        int target_arch, int returns_void, const char *ename,
                        int *handled) {
  (void)ii;
  (void)ename;
  (void)returns_void;
  (void)func;
  (void)program;
  (void)error;
  switch (in->op) {
  case IR_OP_BINARY:
    emit_binary(fn, in);
    break;
  case IR_OP_UNARY:
    emit_unary(fn, in);
    break;
  case IR_OP_CAST: {
    PtxVal target = in->value_type ? descriptor_from_type(in->value_type)
                                   : descriptor_from_typename(in->text);
    MtlcTypeKind target_elem = target.is_ptr ? MTLC_TYPE_VOID : target.elem;
    if (!target.is_ptr &&
        (target_elem == MTLC_TYPE_FLOAT16 || target_elem == MTLC_TYPE_BFLOAT16) &&
        in->is_float) {
      char s[24];
      PtxClass src_cls = (in->float_bits == 64) ? PC_F64 : PC_F32;
      use_as(fn, &in->lhs, src_cls, s);
      target.cls = PC_F32;
      target = destination_value(fn, &in->dest, target);
      char dn[24];
      reg_name(target.cls, target.idx, dn);
      char tmp[24];
      reg_name(PC_B16, new_reg(fn, PC_B16), tmp);
      const char *to = (target_elem == MTLC_TYPE_FLOAT16) ? "f16" : "bf16";
      char f32tmp[24];
      if (src_cls == PC_F64) {
        reg_name(PC_F32, new_reg(fn, PC_F32), f32tmp);
        sb_printf(&fn->body, "\tcvt.rn.f32.f64 %s, %s;\n", f32tmp, s);
        sb_printf(&fn->body, "\tcvt.rn.%s.f32 %s, %s;\n", to, tmp, f32tmp);
      } else {
        sb_printf(&fn->body, "\tcvt.rn.%s.f32 %s, %s;\n", to, tmp, s);
      }
      sb_printf(&fn->body, "\tcvt.f32.%s %s, %s;\n", to, dn, tmp);
      if (in->dest.name) {
        bind_value(fn, in->dest.name, target);
      }
      break;
    }
    char s[24];
    use_as(fn, &in->lhs, target.cls, s);
    PtxVal source = operand_desc(fn, &in->lhs);
    if (target.is_ptr && source.is_ptr &&
        ptx_space_is_plain(in->value_type, in->text) &&
        (source.address_space == MTLC_ADDRESS_SPACE_WORKGROUP ||
         source.address_space == MTLC_ADDRESS_SPACE_PRIVATE ||
         source.address_space == MTLC_ADDRESS_SPACE_GENERIC)) {
      target.address_space = MTLC_ADDRESS_SPACE_GENERIC;
    }
    target = destination_value(fn, &in->dest, target);
    char dn[24];
    reg_name(target.cls, target.idx, dn);
    int converted = target.is_ptr && source.is_ptr &&
                    ptx_convert_space_into(fn, source.address_space,
                                           target.address_space, dn, s);
    if (!converted && strcmp(dn, s) != 0) {
      const char *mt = (target.cls == PC_F32)   ? "f32"
                       : (target.cls == PC_F64) ? "f64"
                       : (target.cls == PC_B64) ? "u64"
                                                : "u32";
      sb_printf(&fn->body, "\tmov.%s %s, %s;\n", mt, dn, s);
    }
    ptx_emit_uniform_check(fn, in, target, target_arch);
    if (in->dest.name) {
      bind_value(fn, in->dest.name, target);
    }
    break;
  }
  default:
    *handled = 0;
    break;
  }
}

static void ptx_call_store_argument(PtxFn *fn, const IRInstruction *in,
                                    size_t a, const MtlcType *argument_type,
                                    const char *parameter,
                                    const char *callee) {
  if (ptx_type_is_aggregate(argument_type)) {
    PtxBinding *record = named_binding(fn, &in->arguments[a]);
    if (!record || !record->val.mem_aggregate) {
      fn_error(fn, "PTX: argument %zu of '%s' is a record with no storage", a,
               callee);
      return;
    }
    size_t size = mtlc_type_size(argument_type);
    size_t alignment = mtlc_type_alignment(argument_type);
    if (!alignment) {
      alignment = 1;
    }
    char source[24];
    reg_name(PC_B64, record->val.mem_addr, source);
    sb_printf(&fn->declarations, "\t.param .align %zu .b8 %s[%zu];\n",
              alignment, parameter, size);
    ptx_block_copy(fn, ".param", parameter, ".local", source, size, alignment);
    return;
  }
  PtxVal argument_desc = descriptor_from_type(argument_type);
  char value[24];
  use_as(fn, &in->arguments[a], argument_desc.cls, value);
  PtxVal passed = operand_desc(fn, &in->arguments[a]);
  if (passed.is_ptr && argument_desc.is_ptr &&
      ptx_space_is_plain(argument_type, NULL)) {
    ptx_convert_space(fn, passed.address_space, MTLC_ADDRESS_SPACE_GENERIC,
                      value);
  } else {
    ptx_generic_address(fn, &in->arguments[a], value);
  }
  sb_printf(&fn->declarations, "\t.param .%s %s;\n",
            device_param_storage_type(argument_desc), parameter);
  sb_printf(&fn->body, "\tst.param.%s [%s], %s;\n",
            device_param_storage_type(argument_desc), parameter, value);
}

static void ptx_call_bind_record_result(PtxFn *fn, const IRInstruction *in,
                                        PtxVal result_desc,
                                        const char *return_parameter,
                                        const char *result_storage) {
  char pointer[24];
  PtxBinding *home = in->dest.kind == IR_OPERAND_SYMBOL
                         ? named_binding(fn, &in->dest)
                         : NULL;
  if (home && home->val.mem_local && home->val.mem_aggregate &&
      home->val.mem_size == result_desc.mem_size) {
    reg_name(PC_B64, home->val.mem_addr, pointer);
    ptx_block_copy(fn, ".local", pointer, ".param", return_parameter,
                   result_desc.mem_size, result_desc.mem_align);
    return;
  }
  result_desc.mem_addr = new_reg(fn, PC_B64);
  reg_name(PC_B64, result_desc.mem_addr, pointer);
  sb_printf(&fn->body, "\tmov.u64 %s, %s;\n", pointer, result_storage);
  ptx_block_copy(fn, ".local", pointer, ".param", return_parameter,
                 result_desc.mem_size, result_desc.mem_align);
  if (in->dest.name) {
    bind_value(fn, in->dest.name, result_desc);
  }
}

static void ptx_emit_general_call(IRProgram *program, IRFunction *func,
                                  PtxFn *fn, const IRInstruction *in,
                                  char **error, const char *ename) {
  (void)error;
  (void)ename;
  const char *callee = in->text;
  MtlcIntrinsic intrinsic = in->intrinsic;

  (void)program;
  (void)func;
  (void)callee;
    if (intrinsic == MTLC_INTRINSIC_NONE) {
      IRFunction *callee_function = ptx_lookup_function(program, callee);
      const IRModuleSymbol *callee_symbol =
          ir_program_lookup_symbol(program, callee);
      if (!callee_function || !callee_symbol ||
          callee_symbol->kind != IR_MODSYM_FUNCTION) {
        fn_error(fn, "PTX: device call target '%s' has no definition",
                 callee ? callee : "?");
        return;
      }
      if (fn->reported)
        ir_explain_device_add(func->name, IR_DEVICE_CALLS_KEPT, 1);
      if (in->argument_count != callee_function->parameter_count ||
          in->argument_count != callee_symbol->param_count) {
        fn_error(fn,
                 "PTX: device call '%s' expects %zu arguments, received %zu",
                 callee, callee_symbol->param_count, in->argument_count);
        return;
      }

      char callee_name[256];
      sanitize_into(callee, callee_name, sizeof(callee_name));
      size_t call_id = fn->call_count++;
      char **argument_params =
          calloc(in->argument_count ? in->argument_count : 1,
                 sizeof(*argument_params));
      if (!argument_params) {
        fn_error(fn, "PTX: out of memory lowering device call '%s'", callee);
        return;
      }
      for (size_t a = 0; a < in->argument_count && !fn->error; a++) {
        char parameter[320];
        snprintf(parameter, sizeof(parameter), "__mtlc_call_%zu_arg_%zu",
                 call_id, a);
        argument_params[a] = strdup(parameter);
        if (!argument_params[a]) {
          fn_error(fn, "PTX: out of memory lowering device call '%s'",
                   callee);
          break;
        }
        ptx_call_store_argument(fn, in, a, callee_symbol->param_types[a],
                                parameter, callee);
      }

      const MtlcType *callee_return_type = ptx_function_return_type(
          program, callee_function, callee_symbol);
      int callee_returns_void = ptx_type_is_void(
          callee_return_type, callee_function->return_type_name);
      int result_is_record = ptx_type_is_aggregate(callee_return_type);
      PtxVal result_desc = callee_returns_void
                               ? (PtxVal){0}
                               : descriptor_from_type(callee_return_type);
      char return_parameter[320] = {0};
      char result_storage[512] = {0};
      if (result_is_record && !fn->error) {
        result_desc = (PtxVal){0};
        result_desc.cls = PC_B64;
        result_desc.elem = MTLC_TYPE_VOID;
        result_desc.mem_local = 1;
        result_desc.mem_aggregate = 1;
        result_desc.mem_size = mtlc_type_size(callee_return_type);
        result_desc.mem_align = mtlc_type_alignment(callee_return_type);
        if (!result_desc.mem_align) {
          result_desc.mem_align = 1;
        }
        char raw[512];
        snprintf(raw, sizeof(raw), "__mtlc_call_%zu_ret_local", call_id);
        sanitize_into(raw, result_storage, sizeof(result_storage));
        sb_printf(&fn->declarations, "\t.local .align %zu .b8 %s[%zu];\n",
                  result_desc.mem_align, result_storage,
                  result_desc.mem_size);
      }
      if (!callee_returns_void && !fn->error) {
        snprintf(return_parameter, sizeof(return_parameter),
                 "__mtlc_call_%zu_ret", call_id);
        if (result_is_record) {
          sb_printf(&fn->declarations, "\t.param .align %zu .b8 %s[%zu];\n",
                    result_desc.mem_align, return_parameter,
                    result_desc.mem_size);
        } else {
          sb_printf(&fn->declarations, "\t.param .%s %s;\n",
                    device_param_storage_type(result_desc), return_parameter);
        }
      }
      if (!fn->error) {
        if (callee_returns_void) {
          sb_printf(&fn->body, "\tcall.uni %s, (", callee_name);
        } else {
          sb_printf(&fn->body, "\tcall.uni (%s), %s, (", return_parameter,
                    callee_name);
        }
        for (size_t a = 0; a < in->argument_count; a++) {
          sb_printf(&fn->body, "%s%s", a ? ", " : "", argument_params[a]);
        }
        sb_puts(&fn->body, ");\n");

        if (callee_returns_void) {
          if (in->dest.name) {
            fn_error(fn, "PTX: void device call '%s' has a result", callee);
          }
        } else if (result_is_record) {
          ptx_call_bind_record_result(fn, in, result_desc, return_parameter,
                                      result_storage);
        } else if (in->dest.name) {
          result_desc = destination_value(fn, &in->dest, result_desc);
          char result[24];
          reg_name(result_desc.cls, result_desc.idx, result);
          sb_printf(&fn->body, "\tld.param.%s %s, [%s];\n",
                    device_param_storage_type(result_desc), result,
                    return_parameter);
          bind_value(fn, in->dest.name, result_desc);
        }
      }
      for (size_t a = 0; a < in->argument_count; a++) {
        free(argument_params[a]);
      }
      free(argument_params);
    } else {
      fn_error(fn, "PTX: unsupported call '%s'", callee ? callee : "?");
    }
}

static void ptx_emit_atomic_intrinsic(IRProgram *program, IRFunction *func,
                                      PtxFn *fn, const IRInstruction *in,
                                      char **error, const char *ename,
                                      int *handled) {
  const char *callee = in->text;
  MtlcIntrinsic intrinsic = in->intrinsic;

  (void)program;
  (void)func;
  (void)callee;
  (void)handled;
    if (ir_intrinsic_is_atomic(intrinsic) &&
               in->argument_count >=
                   (size_t)ir_intrinsic_arity(intrinsic)) {
      int is64 =
          ir_intrinsic_atomic_value_kind(intrinsic) == MTLC_TYPE_UINT64;
      int is_cas = ir_intrinsic_is_compare_exchange(intrinsic);
      int is_load = ir_intrinsic_is_atomic_load(intrinsic);
      int is_store = ir_intrinsic_is_atomic_store(intrinsic);
      int is_sub = intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_SUB_U32 ||
                   intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_SUB_U64;
      const char *opn =
          intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_ADD_U32 ||
                  intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_ADD_U64 || is_sub
              ? "add"
          : intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_MIN_U32 ||
                  intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_MIN_U64
              ? "min"
          : intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_MAX_U32 ||
                  intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_MAX_U64
              ? "max"
          : intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_AND_U32 ||
                  intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_AND_U64
              ? "and"
          : intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_OR_U32 ||
                  intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_OR_U64
              ? "or"
          : intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_XOR_U32 ||
                  intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_XOR_U64
              ? "xor"
          : is_cas ? "cas"
                   : "exch";
      int bit_type = is_cas ||
                     intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_AND_U32 ||
                     intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_AND_U64 ||
                     intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_OR_U32 ||
                     intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_OR_U64 ||
                     intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_XOR_U32 ||
                     intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_XOR_U64 ||
                     intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_EXCHANGE_U32 ||
                     intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_EXCHANGE_U64;
      const char *sem = ptx_atomic_order(in->memory_order);
      const char *scope = ptx_atomic_scope(in->memory_scope);
      const char *space = ptx_atomic_space(in->address_space);
      PtxClass vc = is64 ? PC_B64 : PC_B32;
      int elem = is64 ? 8 : 4;
      char bufr[24], idxr[24], valr[24] = {0};
      use_as(fn, &in->arguments[0], PC_B64, bufr);
      use_as(fn, &in->arguments[1], PC_B64, idxr);
      if (!is_load) use_as(fn, &in->arguments[2], vc, valr);
      if (is_sub) {
        int neg = new_reg(fn, vc);
        char negn[24];
        reg_name(vc, neg, negn);
        sb_printf(&fn->body, "\tneg.%s %s, %s;\n",
                  is64 ? "s64" : "s32", negn, valr);
        strcpy(valr, negn);
      }
      int offset = new_reg(fn, PC_B64);
      int addr = new_reg(fn, PC_B64);
      char offn[24], an[24];
      reg_name(PC_B64, offset, offn);
      reg_name(PC_B64, addr, an);
      sb_printf(&fn->body, "\tmul.lo.u64 %s, %s, %d;\n", offn, idxr, elem);
      sb_printf(&fn->body, "\tadd.u64 %s, %s, %s;\n", an, bufr, offn);
      PtxVal dv = {.cls = vc, .is_unsigned = 1};
      char dn[24] = {0};
      if (!is_store) {
        dv = destination_value(fn, &in->dest, dv);
        reg_name(vc, dv.idx, dn);
      }
      int failure_valid =
          !is_cas ||
          (in->failure_memory_order != MTLC_MEMORY_ORDER_RELEASE &&
           in->failure_memory_order != MTLC_MEMORY_ORDER_ACQ_REL &&
           ((in->memory_order == MTLC_MEMORY_ORDER_RELAXED &&
             in->failure_memory_order == MTLC_MEMORY_ORDER_RELAXED) ||
            ((in->memory_order == MTLC_MEMORY_ORDER_ACQUIRE ||
              in->memory_order == MTLC_MEMORY_ORDER_ACQ_REL) &&
             (in->failure_memory_order == MTLC_MEMORY_ORDER_RELAXED ||
              in->failure_memory_order == MTLC_MEMORY_ORDER_ACQUIRE)) ||
            (in->memory_order == MTLC_MEMORY_ORDER_RELEASE &&
             in->failure_memory_order == MTLC_MEMORY_ORDER_RELAXED) ||
            in->memory_order == MTLC_MEMORY_ORDER_SEQ_CST));
      int order_valid =
          (!is_load || in->memory_order == MTLC_MEMORY_ORDER_RELAXED ||
           in->memory_order == MTLC_MEMORY_ORDER_ACQUIRE ||
           in->memory_order == MTLC_MEMORY_ORDER_SEQ_CST) &&
          (!is_store || in->memory_order == MTLC_MEMORY_ORDER_RELAXED ||
           in->memory_order == MTLC_MEMORY_ORDER_RELEASE ||
           in->memory_order == MTLC_MEMORY_ORDER_SEQ_CST);
      if (!sem || !scope || !space || !failure_valid || !order_valid ||
          (in->address_space == MTLC_ADDRESS_SPACE_WORKGROUP &&
           in->memory_scope > MTLC_MEMORY_SCOPE_WORKGROUP)) {
        fn_error(fn,
                 "PTX: invalid atomic memory contract (space=%d success=%d failure=%d scope=%d)",
                 (int)in->address_space, (int)in->memory_order,
                 (int)in->failure_memory_order, (int)in->memory_scope);
      } else {
        if (in->memory_order == MTLC_MEMORY_ORDER_SEQ_CST) {
          sb_printf(&fn->body, "\tfence.sc.%s;\n", scope);
        }
        const char *type =
            bit_type ? (is64 ? "b64" : "b32")
                     : (is64 ? "u64" : "u32");
        if (is_load) {
          sb_printf(&fn->body, "\tld.%s.%s%s.%s %s, [%s];\n", sem,
                    scope, space, type, dn, an);
        } else if (is_store) {
          const char *store_sem =
              in->memory_order == MTLC_MEMORY_ORDER_SEQ_CST ? "relaxed"
                                                             : sem;
          sb_printf(&fn->body, "\tst.%s.%s%s.%s [%s], %s;\n", store_sem,
                    scope, space, type, an, valr);
        } else if (is_cas) {
          char desired[24];
          use_as(fn, &in->arguments[3], vc, desired);
          sb_printf(&fn->body,
                    "\tatom.%s.%s%s.%s.%s %s, [%s], %s, %s;\n",
                    sem, scope, space, opn, type, dn, an, valr, desired);
        } else {
          sb_printf(&fn->body, "\tatom.%s.%s%s.%s.%s %s, [%s], %s;\n",
                    sem, scope, space, opn, type, dn, an, valr);
        }
      }
      if (in->dest.name && !is_store)
        bind_value(fn, in->dest.name, dv);
    } else {
      ptx_emit_general_call(program, func, fn, in, error, ename);
  }
}

// A workgroup pointer operand as a 32-bit shared-window address. Only
// pointers whose type names workgroup memory qualify: a generic pointer
// would need a run-time conversion these instructions do not take.
static int ptx_shared_address_u32(PtxFn *fn, const IROperand *operand,
                                  const char *what, char out[24]) {
  PtxVal desc = operand_desc(fn, operand);
  if (!desc.is_ptr || desc.address_space != MTLC_ADDRESS_SPACE_WORKGROUP) {
    fn_error(fn, "PTX: %s must be a workgroup (shared) pointer", what);
    return 0;
  }
  char wide[24];
  use_as(fn, operand, PC_B64, wide);
  reg_name(PC_B32, new_reg(fn, PC_B32), out);
  sb_printf(&fn->body, "\tcvt.u32.u64 %s, %s;\n", out, wide);
  return 1;
}

// Transaction barriers (mbarrier) and tensor-map tile loads, the
// asynchronous staging of sm_90 and newer:
//   mbarrier_init(bar, count)             arrivals that complete a phase
//   fence_mbarrier_init()                 publish inits to the async proxy
//   mbarrier_arrive_expect_tx(bar, bytes) one arrival, `bytes` to come
//   mbarrier_wait_parity(bar, parity)     block until that phase completes
//   tma_load_2d(dst, map, c0, c1, bar)    one tile of the tensor map's box
//                                         at element coordinates (c0, c1),
//                                         completing `bar`'s transaction
//   fence_proxy_async()                   order generic workgroup accesses
//                                         before later async-proxy ones
//   tensormap_acquire(map)                acquire a tensor map in global
//                                         memory for the tensor-map proxy
// bar is a uint64 in workgroup memory; map a CUtensorMap in global memory,
// 64-byte aligned. Waiting is acquire, arrival release, at CTA scope.
static void ptx_emit_async_barrier_intrinsic(PtxFn *fn,
                                             const IRInstruction *in,
                                             MtlcIntrinsic intrinsic) {
  if (fn->target_arch < 90 || !ptx_version_at_least(fn, 8, 0)) {
    fn_error(fn, "PTX: transaction barriers and tensor-map loads require sm_90 and PTX 8.0 or newer");
    return;
  }
  char bar[24], value[24], map[24], dst[24], c0[24], c1[24];
  switch (intrinsic) {
  case MTLC_INTRINSIC_GPU_MBARRIER_INIT:
    if (in->argument_count < 2 ||
        !ptx_shared_address_u32(fn, &in->arguments[0], "mbarrier_init's barrier", bar))
      return;
    use_as(fn, &in->arguments[1], PC_B32, value);
    sb_printf(&fn->body, "\tmbarrier.init.shared::cta.b64 [%s], %s;\n", bar,
              value);
    return;
  case MTLC_INTRINSIC_GPU_MBARRIER_ARRIVE_EXPECT_TX:
    if (in->argument_count < 2 ||
        !ptx_shared_address_u32(fn, &in->arguments[0], "mbarrier_arrive_expect_tx's barrier", bar))
      return;
    use_as(fn, &in->arguments[1], PC_B32, value);
    sb_printf(&fn->body,
              "\tmbarrier.arrive.expect_tx.release.cta.shared::cta.b64 _, [%s], %s;\n",
              bar, value);
    return;
  case MTLC_INTRINSIC_GPU_MBARRIER_WAIT_PARITY: {
    if (in->argument_count < 2 ||
        !ptx_shared_address_u32(fn, &in->arguments[0], "mbarrier_wait_parity's barrier", bar))
      return;
    use_as(fn, &in->arguments[1], PC_B32, value);
    char done[24];
    reg_name(PC_PRED, new_reg(fn, PC_PRED), done);
    unsigned long long id = (unsigned long long)fn->call_count++;
    sb_printf(&fn->body,
              "mtlc_mbarrier_wait_%llu:\n"
              "\tmbarrier.try_wait.parity.acquire.cta.shared::cta.b64 %s, [%s], %s;\n"
              "\t@!%s bra mtlc_mbarrier_wait_%llu;\n",
              id, done, bar, value, done, id);
    return;
  }
  case MTLC_INTRINSIC_GPU_FENCE_MBARRIER_INIT:
    sb_puts(&fn->body, "\tfence.mbarrier_init.release.cluster;\n");
    return;
  case MTLC_INTRINSIC_GPU_FENCE_PROXY_ASYNC:
    sb_puts(&fn->body, "\tfence.proxy.async.shared::cta;\n");
    return;
  case MTLC_INTRINSIC_GPU_TMA_LOAD_2D:
    if (in->argument_count < 5 ||
        !ptx_shared_address_u32(fn, &in->arguments[0], "tma_load_2d's destination", dst) ||
        !ptx_shared_address_u32(fn, &in->arguments[4], "tma_load_2d's barrier", bar))
      return;
    use_as(fn, &in->arguments[1], PC_B64, map);
    use_as(fn, &in->arguments[2], PC_B32, c0);
    use_as(fn, &in->arguments[3], PC_B32, c1);
    // The CTA-local destination form (PTX 8.6) where there is one: with a
    // shared::cluster destination ptxas guards every load with a check of
    // the address's CTA rank and a called slow path for a remote CTA, and
    // the call made ptxas spill and shuffle the registers of everything
    // live around it (measured: ~3 moves per MMA in a GEMM's K loop).
    sb_printf(&fn->body,
              "\tcp.async.bulk.tensor.2d.%s.global.tile.mbarrier::complete_tx::bytes [%s], [%s, {%s, %s}], [%s];\n",
              ptx_version_at_least(fn, 8, 6) ? "shared::cta" : "shared::cluster",
              dst, map, c0, c1, bar);
    return;
  case MTLC_INTRINSIC_GPU_TENSORMAP_ACQUIRE:
    if (in->argument_count < 1) return;
    use_as(fn, &in->arguments[0], PC_B64, map);
    sb_printf(&fn->body,
              "\tfence.proxy.tensormap::generic.acquire.gpu [%s], 128;\n", map);
    return;
  default:
    fn_error(fn, "PTX: unknown transaction-barrier intrinsic");
    return;
  }
}

static void ptx_emit_wide_intrinsic(IRProgram *program, IRFunction *func,
                                    PtxFn *fn, const IRInstruction *in,
                                    char **error, const char *ename,
                                    int *handled) {
  const char *callee = in->text;
  MtlcIntrinsic intrinsic = in->intrinsic;

  (void)program;
  (void)func;
  (void)callee;
  (void)handled;
    if (ptx_intrinsic_is_print(intrinsic) &&
               in->argument_count >= 1) {
      const char *format = ptx_literal_string(fn->function, &in->arguments[0]);
      if (!format) {
        fn_error(fn, "PTX: a kernel print needs a literal format string");
      } else {
        int index = ptx_string_index(format);
        if (index < 0) {
          fn_error(fn, "PTX: kernel print format was not interned");
        } else {
          size_t site = fn->call_count++;
          int buffer_bytes =
              intrinsic == MTLC_INTRINSIC_GPU_PRINT      ? 0
              : intrinsic == MTLC_INTRINSIC_GPU_PRINT_I32 ? 4
              : intrinsic == MTLC_INTRINSIC_GPU_PRINT_F32 ? 8
                                                          : 8;
          if (buffer_bytes > 0) {
            sb_printf(&fn->declarations,
                      "\t.local .align 8 .b8 $mtlc_print_args_%zu[%d];\n",
                      site, buffer_bytes);
          }
          int format_register = new_reg(fn, PC_B64);
          char format_name[24];
          reg_name(PC_B64, format_register, format_name);
          sb_printf(&fn->body, "\tmov.u64 %s, $mtlc_str_%d;\n", format_name,
                    index);
          sb_printf(&fn->body, "\tcvta.global.u64 %s, %s;\n", format_name,
                    format_name);
          int args_register = new_reg(fn, PC_B64);
          char args_name[24];
          reg_name(PC_B64, args_register, args_name);
          if (buffer_bytes > 0) {
            sb_printf(&fn->body, "\tmov.u64 %s, $mtlc_print_args_%zu;\n",
                      args_name, site);
            sb_printf(&fn->body, "\tcvta.local.u64 %s, %s;\n", args_name,
                      args_name);
            if (intrinsic == MTLC_INTRINSIC_GPU_PRINT_I32) {
              char value[24];
              use_as(fn, &in->arguments[1], PC_B32, value);
              sb_printf(&fn->body, "\tst.u32 [%s], %s;\n", args_name, value);
            } else if (intrinsic == MTLC_INTRINSIC_GPU_PRINT_F32) {
              char value[24];
              use_as(fn, &in->arguments[1], PC_F32, value);
              int widened = new_reg(fn, PC_F64);
              char widened_name[24];
              reg_name(PC_F64, widened, widened_name);
              sb_printf(&fn->body, "\tcvt.f64.f32 %s, %s;\n", widened_name,
                        value);
              sb_printf(&fn->body, "\tst.f64 [%s], %s;\n", args_name,
                        widened_name);
            } else {
              char first[24];
              char second[24];
              use_as(fn, &in->arguments[1], PC_B32, first);
              use_as(fn, &in->arguments[2], PC_B32, second);
              sb_printf(&fn->body, "\tst.u32 [%s], %s;\n", args_name, first);
              sb_printf(&fn->body, "\tst.u32 [%s+4], %s;\n", args_name,
                        second);
            }
          } else {
            sb_printf(&fn->body, "\tmov.u64 %s, 0;\n", args_name);
          }
          sb_puts(&fn->body, "\t{\n");
          sb_puts(&fn->body, "\t.param .b64 $mtlc_print_fmt;\n");
          sb_puts(&fn->body, "\t.param .b64 $mtlc_print_buf;\n");
          sb_puts(&fn->body, "\t.param .b32 $mtlc_print_ret;\n");
          sb_printf(&fn->body, "\tst.param.b64 [$mtlc_print_fmt+0], %s;\n",
                    format_name);
          sb_printf(&fn->body, "\tst.param.b64 [$mtlc_print_buf+0], %s;\n",
                    args_name);
          sb_puts(&fn->body,
                  "\tcall.uni ($mtlc_print_ret), vprintf, "
                  "($mtlc_print_fmt, $mtlc_print_buf);\n");
          sb_puts(&fn->body, "\t}\n");
        }
      }
    } else if ((intrinsic == MTLC_INTRINSIC_GPU_DP2A_LO_U32 ||
                intrinsic == MTLC_INTRINSIC_GPU_DP2A_LO_S32 ||
                intrinsic == MTLC_INTRINSIC_GPU_DP2A_HI_U32 ||
                intrinsic == MTLC_INTRINSIC_GPU_DP2A_HI_S32) &&
               in->argument_count >= 3) {
      int is_signed = intrinsic == MTLC_INTRINSIC_GPU_DP2A_LO_S32 ||
                      intrinsic == MTLC_INTRINSIC_GPU_DP2A_HI_S32;
      int is_high = intrinsic == MTLC_INTRINSIC_GPU_DP2A_HI_U32 ||
                    intrinsic == MTLC_INTRINSIC_GPU_DP2A_HI_S32;
      char a[24];
      char b[24];
      char c[24];
      use_as(fn, &in->arguments[0], PC_B32, a);
      use_as(fn, &in->arguments[1], PC_B32, b);
      use_as(fn, &in->arguments[2], PC_B32, c);
      PtxVal dv = {.cls = PC_B32, .is_unsigned = !is_signed};
      dv = destination_value(fn, &in->dest, dv);
      char dn[24];
      reg_name(PC_B32, dv.idx, dn);
      sb_printf(&fn->body, "\tdp2a.%s.%s %s, %s, %s, %s;\n",
                is_high ? "hi" : "lo", is_signed ? "s32.s32" : "u32.u32", dn,
                a, b, c);
      if (in->dest.name)
        bind_value(fn, in->dest.name, dv);
    } else if (intrinsic == MTLC_INTRINSIC_GPU_PRMT_B32 &&
               in->argument_count >= 3) {
      char a[24];
      char b[24];
      char sel[24];
      use_as(fn, &in->arguments[0], PC_B32, a);
      use_as(fn, &in->arguments[1], PC_B32, b);
      use_as(fn, &in->arguments[2], PC_B32, sel);
      PtxVal dv = {.cls = PC_B32, .is_unsigned = 1};
      dv = destination_value(fn, &in->dest, dv);
      char dn[24];
      reg_name(PC_B32, dv.idx, dn);
      sb_printf(&fn->body, "\tprmt.b32 %s, %s, %s, %s;\n", dn, a, b, sel);
      if (in->dest.name)
        bind_value(fn, in->dest.name, dv);
    } else if (intrinsic >= MTLC_INTRINSIC_GPU_MBARRIER_INIT &&
               intrinsic <= MTLC_INTRINSIC_GPU_TENSORMAP_ACQUIRE) {
      ptx_emit_async_barrier_intrinsic(fn, in, intrinsic);
    } else if ((intrinsic == MTLC_INTRINSIC_GPU_LOAD4_F32 ||
                intrinsic == MTLC_INTRINSIC_GPU_LOAD4_U32 ||
                intrinsic == MTLC_INTRINSIC_GPU_STORE4_F32 ||
                intrinsic == MTLC_INTRINSIC_GPU_STORE4_U32) &&
               in->argument_count >= 2) {
      int is_load = intrinsic == MTLC_INTRINSIC_GPU_LOAD4_F32 ||
                    intrinsic == MTLC_INTRINSIC_GPU_LOAD4_U32;
      int is_float = intrinsic == MTLC_INTRINSIC_GPU_LOAD4_F32 ||
                     intrinsic == MTLC_INTRINSIC_GPU_STORE4_F32;
      PtxClass cls = is_float ? PC_F32 : PC_B32;
      const char *suffix = is_float ? "f32" : "u32";
      const IROperand *vector_operand = &in->arguments[0];
      const IROperand *scalar_operand = &in->arguments[1];
      PtxVal vector_desc = operand_desc(fn, vector_operand);
      PtxVal scalar_desc = operand_desc(fn, scalar_operand);
      const char *vector_space = ptx_memory_space(vector_desc.address_space);
      const char *scalar_space = ptx_memory_space(scalar_desc.address_space);
      char vector_address[24];
      char scalar_address[24];
      use_as(fn, vector_operand, PC_B64, vector_address);
      use_as(fn, scalar_operand, PC_B64, scalar_address);
      if (!vector_space || !scalar_space) {
        fn_error(fn, "PTX: invalid address space in a 4-wide transfer");
      } else {
        int lanes[4];
        char lane_names[4][24];
        for (int lane = 0; lane < 4; lane++) {
          lanes[lane] = new_reg(fn, cls);
          reg_name(cls, lanes[lane], lane_names[lane]);
        }
        if (is_load) {
          sb_printf(&fn->body, "\tld%s.v4.%s {%s, %s, %s, %s}, [%s];\n",
                    vector_space, suffix, lane_names[0], lane_names[1],
                    lane_names[2], lane_names[3], vector_address);
          for (int lane = 0; lane < 4; lane++) {
            sb_printf(&fn->body, "\tst%s.%s [%s+%d], %s;\n", scalar_space,
                      suffix, scalar_address, lane * 4, lane_names[lane]);
          }
        } else {
          for (int lane = 0; lane < 4; lane++) {
            sb_printf(&fn->body, "\tld%s.%s %s, [%s+%d];\n", scalar_space,
                      suffix, lane_names[lane], scalar_address, lane * 4);
          }
          sb_printf(&fn->body, "\tst%s.v4.%s [%s], {%s, %s, %s, %s};\n",
                    vector_space, suffix, vector_address, lane_names[0],
                    lane_names[1], lane_names[2], lane_names[3]);
        }
      }
    } else if (intrinsic >= MTLC_INTRINSIC_GPU_SQRT_F32 &&
               intrinsic <= MTLC_INTRINSIC_GPU_EXP_F32 &&
               in->argument_count >= 1) {
      char a[24];
      use_as(fn, &in->arguments[0], PC_F32, a);
      PtxVal dv = {.cls = PC_F32};
      dv = destination_value(fn, &in->dest, dv);
      char dn[24];
      reg_name(PC_F32, dv.idx, dn);
      if (intrinsic == MTLC_INTRINSIC_GPU_SQRT_F32) {
        sb_printf(&fn->body, "\tsqrt.rn.f32 %s, %s;\n", dn, a);
      } else if (intrinsic == MTLC_INTRINSIC_GPU_RSQRT_F32) {
        sb_printf(&fn->body, "\trsqrt.approx.f32 %s, %s;\n", dn, a);
      } else if (intrinsic == MTLC_INTRINSIC_GPU_ABS_F32) {
        sb_printf(&fn->body, "\tabs.f32 %s, %s;\n", dn, a);
      } else if (intrinsic == MTLC_INTRINSIC_GPU_SIN_F32) {
        sb_printf(&fn->body, "\tsin.approx.f32 %s, %s;\n", dn, a);
      } else if (intrinsic == MTLC_INTRINSIC_GPU_COS_F32) {
        sb_printf(&fn->body, "\tcos.approx.f32 %s, %s;\n", dn, a);
      } else if (intrinsic == MTLC_INTRINSIC_GPU_LOG_F32) {
        int t = new_reg(fn, PC_F32);
        char tn[24];
        reg_name(PC_F32, t, tn);
        sb_printf(&fn->body, "\tlg2.approx.f32 %s, %s;\n", tn, a);
        sb_printf(&fn->body, "\tmul.%sf32 %s, %s, 0f3F317218;\n",
                  fn->rounded_arithmetic ? "rn." : "", dn, tn);
      } else {
        int t = new_reg(fn, PC_F32);
        char tn[24];
        reg_name(PC_F32, t, tn);
        sb_printf(&fn->body, "\tmul.%sf32 %s, %s, 0f3FB8AA3B;\n",
                  fn->rounded_arithmetic ? "rn." : "", tn, a);
        sb_printf(&fn->body, "\tex2.approx.f32 %s, %s;\n", dn, tn);
      }
      if (in->dest.name)
        bind_value(fn, in->dest.name, dv);
    } else {
      ptx_emit_atomic_intrinsic(program, func, fn, in, error, ename, handled);
    }
}

static void ptx_emit_f2bf(PtxFn *fn, const IRInstruction *in) {
  char a[24];
  char dn[24];
  use_as(fn, &in->arguments[0], PC_F32, a);
  if (fn->target_arch >= 80 && ptx_version_at_least(fn, 7, 0)) {
    int hidx = new_reg(fn, PC_B16);
    char hn[24];
    reg_name(PC_B16, hidx, hn);
    sb_printf(&fn->body, "\tcvt.rn.bf16.f32 %s, %s;\n", hn, a);
    PtxVal dv = {.cls = PC_B32, .is_unsigned = 1};
    dv = destination_value(fn, &in->dest, dv);
    reg_name(PC_B32, dv.idx, dn);
    sb_printf(&fn->body, "\tcvt.u32.u16 %s, %s;\n", dn, hn);
    if (in->dest.name) bind_value(fn, in->dest.name, dv);
    return;
  }
  int bits = new_reg(fn, PC_B32);
  int carry = new_reg(fn, PC_B32);
  int rounded = new_reg(fn, PC_B32);
  char bits_name[24];
  char carry_name[24];
  char rounded_name[24];
  reg_name(PC_B32, bits, bits_name);
  reg_name(PC_B32, carry, carry_name);
  reg_name(PC_B32, rounded, rounded_name);
  sb_printf(&fn->body, "\tmov.b32 %s, %s;\n", bits_name, a);
  sb_printf(&fn->body, "\tshr.u32 %s, %s, 16;\n", carry_name, bits_name);
  sb_printf(&fn->body, "\tand.b32 %s, %s, 1;\n", carry_name, carry_name);
  sb_printf(&fn->body, "\tadd.u32 %s, %s, 32767;\n", rounded_name, carry_name);
  sb_printf(&fn->body, "\tadd.u32 %s, %s, %s;\n", rounded_name, rounded_name,
            bits_name);
  PtxVal dv = {.cls = PC_B32, .is_unsigned = 1};
  dv = destination_value(fn, &in->dest, dv);
  reg_name(PC_B32, dv.idx, dn);
  sb_printf(&fn->body, "\tshr.u32 %s, %s, 16;\n", dn, rounded_name);
  if (in->dest.name) bind_value(fn, in->dest.name, dv);
}

static void ptx_emit_numeric_intrinsic(IRProgram *program, IRFunction *func,
                                       PtxFn *fn, const IRInstruction *in,
                                       char **error, const char *ename,
                                       int *handled) {
  const char *callee = in->text;
  MtlcIntrinsic intrinsic = in->intrinsic;

  (void)program;
  (void)func;
  (void)callee;
  (void)handled;
    if (intrinsic == MTLC_INTRINSIC_GPU_WORKGROUP_BARRIER) {
      sb_puts(&fn->body, "\tbar.sync 0;\n");
    } else if (intrinsic == MTLC_INTRINSIC_GPU_F16_BITS_TO_F32 &&
               in->argument_count >= 1) {
      char a[24];
      use_as(fn, &in->arguments[0], PC_B32, a);
      int hidx = new_reg(fn, PC_B16);
      char hn[24];
      reg_name(PC_B16, hidx, hn);
      sb_printf(&fn->body, "\tcvt.u16.u32 %s, %s;\n", hn, a);
      PtxVal dv = {.cls = PC_F32};
      dv = destination_value(fn, &in->dest, dv);
      char dn[24];
      reg_name(PC_F32, dv.idx, dn);
      sb_printf(&fn->body, "\tcvt.f32.f16 %s, %s;\n", dn, hn);
      if (in->dest.name)
        bind_value(fn, in->dest.name, dv);
    } else if (intrinsic == MTLC_INTRINSIC_GPU_F32_TO_F16_BITS &&
               in->argument_count >= 1) {
      char a[24];
      use_as(fn, &in->arguments[0], PC_F32, a);
      int hidx = new_reg(fn, PC_B16);
      char hn[24];
      reg_name(PC_B16, hidx, hn);
      sb_printf(&fn->body, "\tcvt.rn.f16.f32 %s, %s;\n", hn, a);
      PtxVal dv = {.cls = PC_B32, .is_unsigned = 1};
      dv = destination_value(fn, &in->dest, dv);
      char dn[24];
      reg_name(PC_B32, dv.idx, dn);
      sb_printf(&fn->body, "\tcvt.u32.u16 %s, %s;\n", dn, hn);
      if (in->dest.name)
        bind_value(fn, in->dest.name, dv);
    } else if (intrinsic == MTLC_INTRINSIC_GPU_F32_FROM_BITS &&
               in->argument_count >= 1) {
      char a[24];
      use_as(fn, &in->arguments[0], PC_B32, a);
      PtxVal dv = {.cls = PC_F32};
      dv = destination_value(fn, &in->dest, dv);
      char dn[24];
      reg_name(PC_F32, dv.idx, dn);
      sb_printf(&fn->body, "\tmov.b32 %s, %s;\n", dn, a);
      if (in->dest.name)
        bind_value(fn, in->dest.name, dv);
    } else if (intrinsic == MTLC_INTRINSIC_GPU_F32_TO_BITS &&
               in->argument_count >= 1) {
      char a[24];
      use_as(fn, &in->arguments[0], PC_F32, a);
      PtxVal dv = {.cls = PC_B32, .is_unsigned = 1};
      dv = destination_value(fn, &in->dest, dv);
      char dn[24];
      reg_name(PC_B32, dv.idx, dn);
      sb_printf(&fn->body, "\tmov.b32 %s, %s;\n", dn, a);
      if (in->dest.name)
        bind_value(fn, in->dest.name, dv);
    } else if ((intrinsic == MTLC_INTRINSIC_GPU_DP4A_U32 ||
                intrinsic == MTLC_INTRINSIC_GPU_DP4A_S32) &&
               in->argument_count >= 3) {
      int is_signed = intrinsic == MTLC_INTRINSIC_GPU_DP4A_S32;
      char a[24];
      char b[24];
      char c[24];
      use_as(fn, &in->arguments[0], PC_B32, a);
      use_as(fn, &in->arguments[1], PC_B32, b);
      use_as(fn, &in->arguments[2], PC_B32, c);
      PtxVal dv = {.cls = PC_B32, .is_unsigned = !is_signed};
      dv = destination_value(fn, &in->dest, dv);
      char dn[24];
      reg_name(PC_B32, dv.idx, dn);
      sb_printf(&fn->body, "\tdp4a.%s %s, %s, %s, %s;\n",
                is_signed ? "s32.s32" : "u32.u32", dn, a, b, c);
      if (in->dest.name)
        bind_value(fn, in->dest.name, dv);
    } else if ((intrinsic == MTLC_INTRINSIC_GPU_HADD2 ||
                intrinsic == MTLC_INTRINSIC_GPU_HMUL2 ||
                intrinsic == MTLC_INTRINSIC_GPU_HFMA2) &&
               in->argument_count >= 2) {
      int is_fma = intrinsic == MTLC_INTRINSIC_GPU_HFMA2;
      char a[24];
      char b[24];
      char c[24];
      use_as(fn, &in->arguments[0], PC_B32, a);
      use_as(fn, &in->arguments[1], PC_B32, b);
      if (is_fma) {
        use_as(fn, &in->arguments[2], PC_B32, c);
      }
      PtxVal dv = {.cls = PC_B32, .is_unsigned = 1};
      dv = destination_value(fn, &in->dest, dv);
      char dn[24];
      reg_name(PC_B32, dv.idx, dn);
      if (is_fma) {
        sb_printf(&fn->body, "\tfma.rn.f16x2 %s, %s, %s, %s;\n", dn, a, b, c);
      } else {
        sb_printf(&fn->body, "\t%s.rn.f16x2 %s, %s, %s;\n",
                  intrinsic == MTLC_INTRINSIC_GPU_HADD2 ? "add" : "mul", dn,
                  a, b);
      }
      if (in->dest.name)
        bind_value(fn, in->dest.name, dv);
    } else if ((intrinsic == MTLC_INTRINSIC_GPU_H2F_LO ||
                intrinsic == MTLC_INTRINSIC_GPU_H2F_HI) &&
               in->argument_count >= 1) {
      char a[24];
      use_as(fn, &in->arguments[0], PC_B32, a);
      const char *source = a;
      char shifted_name[24];
      if (intrinsic == MTLC_INTRINSIC_GPU_H2F_HI) {
        int shifted = new_reg(fn, PC_B32);
        reg_name(PC_B32, shifted, shifted_name);
        sb_printf(&fn->body, "\tshr.u32 %s, %s, 16;\n", shifted_name, a);
        source = shifted_name;
      }
      int half = new_reg(fn, PC_B16);
      char half_name[24];
      reg_name(PC_B16, half, half_name);
      sb_printf(&fn->body, "\tcvt.u16.u32 %s, %s;\n", half_name, source);
      PtxVal dv = {.cls = PC_F32};
      dv = destination_value(fn, &in->dest, dv);
      char dn[24];
      reg_name(PC_F32, dv.idx, dn);
      sb_printf(&fn->body, "\tcvt.f32.f16 %s, %s;\n", dn, half_name);
      if (in->dest.name)
        bind_value(fn, in->dest.name, dv);
    } else if (intrinsic == MTLC_INTRINSIC_GPU_F2H2 &&
               in->argument_count >= 2) {
      char lo[24];
      char hi[24];
      use_as(fn, &in->arguments[0], PC_F32, lo);
      use_as(fn, &in->arguments[1], PC_F32, hi);
      int lo_half = new_reg(fn, PC_B16);
      int hi_half = new_reg(fn, PC_B16);
      char lo_name[24];
      char hi_name[24];
      reg_name(PC_B16, lo_half, lo_name);
      reg_name(PC_B16, hi_half, hi_name);
      sb_printf(&fn->body, "\tcvt.rn.f16.f32 %s, %s;\n", lo_name, lo);
      sb_printf(&fn->body, "\tcvt.rn.f16.f32 %s, %s;\n", hi_name, hi);
      PtxVal dv = {.cls = PC_B32, .is_unsigned = 1};
      dv = destination_value(fn, &in->dest, dv);
      char dn[24];
      reg_name(PC_B32, dv.idx, dn);
      sb_printf(&fn->body, "\tmov.b32 %s, {%s, %s};\n", dn, lo_name, hi_name);
      if (in->dest.name)
        bind_value(fn, in->dest.name, dv);
    } else if (intrinsic == MTLC_INTRINSIC_GPU_BF2F &&
               in->argument_count >= 1) {
      char a[24];
      use_as(fn, &in->arguments[0], PC_B32, a);
      int shifted = new_reg(fn, PC_B32);
      char shifted_name[24];
      reg_name(PC_B32, shifted, shifted_name);
      sb_printf(&fn->body, "\tshl.b32 %s, %s, 16;\n", shifted_name, a);
      PtxVal dv = {.cls = PC_F32};
      dv = destination_value(fn, &in->dest, dv);
      char dn[24];
      reg_name(PC_F32, dv.idx, dn);
      sb_printf(&fn->body, "\tmov.b32 %s, %s;\n", dn, shifted_name);
      if (in->dest.name)
        bind_value(fn, in->dest.name, dv);
    } else if (intrinsic == MTLC_INTRINSIC_GPU_WORKGROUP_UNIFORM_U32 &&
               in->argument_count >= 1) {
      char a[24];
      use_as(fn, &in->arguments[0], PC_B32, a);
      PtxVal dv = {.cls = PC_B32, .is_unsigned = 1};
      dv = destination_value(fn, &in->dest, dv);
      char dn[24];
      reg_name(PC_B32, dv.idx, dn);
      sb_printf(&fn->body, "\tmov.b32 %s, %s;\n", dn, a);
      if (in->dest.name)
        bind_value(fn, in->dest.name, dv);
    } else if (intrinsic == MTLC_INTRINSIC_GPU_F2BF &&
               in->argument_count >= 1) {
      ptx_emit_f2bf(fn, in);
    } else if (intrinsic == MTLC_INTRINSIC_GPU_ASSERT &&
               in->argument_count >= 1) {
      if (g_ptx_emit_checks) {
        char condition[24];
        use_as(fn, &in->arguments[0], PC_B32, condition);
        int predicate = new_reg(fn, PC_PRED);
        char predicate_name[24];
        reg_name(PC_PRED, predicate, predicate_name);
        size_t label = fn->call_count++;
        sb_printf(&fn->body, "\tsetp.ne.s32 %s, %s, 0;\n", predicate_name,
                  condition);
        sb_printf(&fn->body, "\t@%s bra $mtlc_assert_ok_%zu;\n",
                  predicate_name, label);
        sb_puts(&fn->body, "\ttrap;\n");
        sb_printf(&fn->body, "$mtlc_assert_ok_%zu:\n", label);
      }
    } else {
      ptx_emit_wide_intrinsic(program, func, fn, in, error, ename, handled);
    }
}

static void ptx_emit_call(IRProgram *program, IRFunction *func, PtxFn *fn,
                        const IRInstruction *in, size_t *ii, char **error,
                        int target_arch, int returns_void, const char *ename,
                        int *handled) {
  (void)ii;
  (void)returns_void;
  (void)target_arch;
  switch (in->op) {
  case IR_OP_CALL: {
    MtlcIntrinsic intrinsic = in->intrinsic;
    const char *sreg = sreg_for_intrinsic(intrinsic);
    if (sreg) {
      PtxVal dv = {0};
      dv.cls = PC_B32;
      dv.is_unsigned = 1;
      dv = destination_value(fn, &in->dest, dv);
      char dn[24];
      reg_name(PC_B32, dv.idx, dn);
      sb_printf(&fn->body, "\tmov.u32 %s, %s;\n", dn, sreg);
      if (in->dest.name) {
        bind_value(fn, in->dest.name, dv);
      }
    } else if (intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_SIZE) {
      PtxVal dv = {.cls = PC_B32, .is_unsigned = 1};
      dv = destination_value(fn, &in->dest, dv);
      char dn[24];
      reg_name(PC_B32, dv.idx, dn);
      sb_printf(&fn->body, "\tmov.u32 %s, 32;\n", dn);
      if (in->dest.name) bind_value(fn, in->dest.name, dv);
    } else if ((intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_BROADCAST_U32 ||
                intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_BROADCAST_F32) &&
               in->argument_count >= 2) {
      int is_float =
          intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_BROADCAST_F32;
      PtxClass cls = is_float ? PC_F32 : PC_B32;
      char value[24], source_lane[24], mask[24], dn[24];
      use_as(fn, &in->arguments[0], cls, value);
      use_as(fn, &in->arguments[1], PC_B32, source_lane);
      int mask_index = new_reg(fn, PC_B32);
      reg_name(PC_B32, mask_index, mask);
      PtxVal dv = {.cls = cls, .is_unsigned = !is_float};
      dv = destination_value(fn, &in->dest, dv);
      reg_name(cls, dv.idx, dn);
      sb_printf(&fn->body, "\tactivemask.b32 %s;\n", mask);
      sb_printf(&fn->body,
                "\tshfl.sync.idx.b32 %s, %s, %s, 0x1f, %s;\n",
                dn, value, source_lane, mask);
      if (in->dest.name) bind_value(fn, in->dest.name, dv);
    } else if ((intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_SHUFFLE_U32 ||
                intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_SHUFFLE_F32) &&
               in->argument_count >= 2) {
      int is_float = intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_SHUFFLE_F32;
      PtxClass cls = is_float ? PC_F32 : PC_B32;
      char value[24], source_lane[24], mask[24], other[24], dn[24];
      char shuffle_ok[24], source_in_range[24], source_active[24];
      char bit[24], active_bits[24], take[24];
      use_as(fn, &in->arguments[0], cls, value);
      use_as(fn, &in->arguments[1], PC_B32, source_lane);
      int mask_index = new_reg(fn, PC_B32);
      int other_index = new_reg(fn, cls);
      int shuffle_ok_index = new_reg(fn, PC_PRED);
      int source_in_range_index = new_reg(fn, PC_PRED);
      int source_active_index = new_reg(fn, PC_PRED);
      int bit_index = new_reg(fn, PC_B32);
      int active_bits_index = new_reg(fn, PC_B32);
      int take_index = new_reg(fn, PC_PRED);
      reg_name(PC_B32, mask_index, mask);
      reg_name(cls, other_index, other);
      reg_name(PC_PRED, shuffle_ok_index, shuffle_ok);
      reg_name(PC_PRED, source_in_range_index, source_in_range);
      reg_name(PC_PRED, source_active_index, source_active);
      reg_name(PC_B32, bit_index, bit);
      reg_name(PC_B32, active_bits_index, active_bits);
      reg_name(PC_PRED, take_index, take);
      PtxVal dv = {.cls = cls, .is_unsigned = !is_float};
      dv = destination_value(fn, &in->dest, dv);
      reg_name(cls, dv.idx, dn);
      sb_printf(&fn->body, "\tactivemask.b32 %s;\n", mask);
      sb_printf(&fn->body,
                "\tshfl.sync.idx.b32 %s|%s, %s, %s, 0x1f, %s;\n",
                other, shuffle_ok, value, source_lane, mask);
      sb_printf(&fn->body, "\tsetp.lt.u32 %s, %s, 32;\n",
                source_in_range, source_lane);
      sb_printf(&fn->body, "\tmov.u32 %s, 0;\n", bit);
      sb_printf(&fn->body, "\t@%s shl.b32 %s, 1, %s;\n",
                source_in_range, bit, source_lane);
      sb_printf(&fn->body, "\tand.b32 %s, %s, %s;\n", active_bits,
                mask, bit);
      sb_printf(&fn->body, "\tsetp.ne.u32 %s, %s, 0;\n", source_active,
                active_bits);
      sb_printf(&fn->body, "\tand.pred %s, %s, %s;\n", take,
                shuffle_ok, source_active);
      sb_printf(&fn->body, "\tmov.%s %s, %s;\n",
                is_float ? "f32" : "u32", dn, value);
      sb_printf(&fn->body, "\t@%s mov.%s %s, %s;\n", take,
                is_float ? "f32" : "u32", dn, other);
      if (in->dest.name) bind_value(fn, in->dest.name, dv);
    } else if (intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_BALLOT_WORD &&
               in->argument_count >= 2) {
      char predicate[24], word[24], mask[24], bits[24], word_zero[24], dn[24];
      use_as(fn, &in->arguments[0], PC_PRED, predicate);
      use_as(fn, &in->arguments[1], PC_B32, word);
      int mask_index = new_reg(fn, PC_B32);
      int bits_index = new_reg(fn, PC_B32);
      int word_zero_index = new_reg(fn, PC_PRED);
      reg_name(PC_B32, mask_index, mask);
      reg_name(PC_B32, bits_index, bits);
      reg_name(PC_PRED, word_zero_index, word_zero);
      PtxVal dv = {.cls = PC_B32, .is_unsigned = 1};
      dv = destination_value(fn, &in->dest, dv);
      reg_name(PC_B32, dv.idx, dn);
      sb_printf(&fn->body, "\tactivemask.b32 %s;\n", mask);
      sb_printf(&fn->body, "\tvote.sync.ballot.b32 %s, %s, %s;\n",
                bits, predicate, mask);
      sb_printf(&fn->body, "\tsetp.eq.u32 %s, %s, 0;\n", word_zero, word);
      sb_printf(&fn->body, "\tmov.u32 %s, 0;\n", dn);
      sb_printf(&fn->body, "\t@%s mov.u32 %s, %s;\n", word_zero, dn, bits);
      if (in->dest.name) bind_value(fn, in->dest.name, dv);
    } else if ((intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_ANY ||
                intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_ALL) &&
               in->argument_count >= 1) {
      char predicate[24], mask[24], dn[24];
      use_as(fn, &in->arguments[0], PC_PRED, predicate);
      int mask_index = new_reg(fn, PC_B32);
      reg_name(PC_B32, mask_index, mask);
      PtxVal dv = {.cls = PC_PRED, .is_unsigned = 1};
      dv = destination_value(fn, &in->dest, dv);
      reg_name(PC_PRED, dv.idx, dn);
      sb_printf(&fn->body, "\tactivemask.b32 %s;\n", mask);
      sb_printf(&fn->body, "\tvote.sync.%s.pred %s, %s, %s;\n",
                intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_ANY ? "any" : "all",
                dn, predicate, mask);
      if (in->dest.name) bind_value(fn, in->dest.name, dv);
    } else if ((intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_ADD_U32 ||
                intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_ADD_F32 ||
                intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MIN_U32 ||
                intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MIN_F32 ||
                intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MAX_U32 ||
                intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MAX_F32) &&
                in->argument_count >= 1) {
      int is_float = intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_ADD_F32 ||
                     intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MIN_F32 ||
                     intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MAX_F32;
      const char *operation =
          (intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MIN_U32 ||
           intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MIN_F32)
              ? "min"
          : (intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MAX_U32 ||
             intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MAX_F32)
              ? "max"
              : "add";
      PtxClass cls = is_float ? PC_F32 : PC_B32;
      char input[24], accum[24], mask[24], lane[24];
      use_as(fn, &in->arguments[0], cls, input);
      PtxVal dv = {.cls = cls, .is_unsigned = !is_float};
      dv = destination_value(fn, &in->dest, dv);
      reg_name(cls, dv.idx, accum);
      sb_printf(&fn->body, "\tmov.%s %s, %s;\n",
                is_float ? "f32" : "u32", accum, input);
      int mask_index = new_reg(fn, PC_B32);
      int lane_index = new_reg(fn, PC_B32);
      reg_name(PC_B32, mask_index, mask);
      reg_name(PC_B32, lane_index, lane);
      sb_printf(&fn->body, "\tactivemask.b32 %s;\n", mask);
      sb_printf(&fn->body, "\tmov.u32 %s, %%laneid;\n", lane);

      static const unsigned offsets[] = {16, 8, 4, 2, 1};
      for (size_t oi = 0; oi < sizeof(offsets) / sizeof(offsets[0]); oi++) {
        unsigned offset = offsets[oi];
        char other[24], source[24], bit[24], active_bits[24];
        char shuffle_ok[24], source_in_range[24], source_active[24], take[24];
        int other_index = new_reg(fn, cls);
        int source_index = new_reg(fn, PC_B32);
        int bit_index = new_reg(fn, PC_B32);
        int active_bits_index = new_reg(fn, PC_B32);
        int shuffle_ok_index = new_reg(fn, PC_PRED);
        int source_in_range_index = new_reg(fn, PC_PRED);
        int source_active_index = new_reg(fn, PC_PRED);
        int take_index = new_reg(fn, PC_PRED);
        reg_name(cls, other_index, other);
        reg_name(PC_B32, source_index, source);
        reg_name(PC_B32, bit_index, bit);
        reg_name(PC_B32, active_bits_index, active_bits);
        reg_name(PC_PRED, shuffle_ok_index, shuffle_ok);
        reg_name(PC_PRED, source_in_range_index, source_in_range);
        reg_name(PC_PRED, source_active_index, source_active);
        reg_name(PC_PRED, take_index, take);
        sb_printf(&fn->body,
                  "\tshfl.sync.down.b32 %s|%s, %s, %u, 0x1f, %s;\n",
                  other, shuffle_ok, accum, offset, mask);
        sb_printf(&fn->body, "\tadd.u32 %s, %s, %u;\n", source, lane,
                  offset);
        sb_printf(&fn->body, "\tsetp.lt.u32 %s, %s, 32;\n",
                  source_in_range, source);
        sb_printf(&fn->body, "\tmov.u32 %s, 0;\n", bit);
        sb_printf(&fn->body, "\t@%s shl.b32 %s, 1, %s;\n",
                  source_in_range, bit, source);
        sb_printf(&fn->body, "\tand.b32 %s, %s, %s;\n", active_bits,
                  mask, bit);
        sb_printf(&fn->body, "\tsetp.ne.u32 %s, %s, 0;\n", source_active,
                  active_bits);
        sb_printf(&fn->body, "\tand.pred %s, %s, %s;\n", take,
                  shuffle_ok, source_active);
        sb_printf(&fn->body, "\t@%s %s.%s %s, %s, %s;\n", take, operation,
                   is_float ? "f32" : "u32", accum, accum, other);
      }
      sb_printf(&fn->body,
                "\tshfl.sync.idx.b32 %s, %s, 0, 0x1f, %s;\n",
                accum, accum, mask);
      if (in->dest.name) bind_value(fn, in->dest.name, dv);
    } else if ((intrinsic ==
                    MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_INCLUSIVE_ADD_U32 ||
                intrinsic ==
                    MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_INCLUSIVE_ADD_F32 ||
                intrinsic ==
                    MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_EXCLUSIVE_ADD_U32 ||
                intrinsic ==
                    MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_EXCLUSIVE_ADD_F32) &&
               in->argument_count >= 1) {
      int is_float =
          intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_INCLUSIVE_ADD_F32 ||
          intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_EXCLUSIVE_ADD_F32;
      int is_exclusive =
          intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_EXCLUSIVE_ADD_U32 ||
          intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_EXCLUSIVE_ADD_F32;
      PtxClass cls = is_float ? PC_F32 : PC_B32;
      char input[24], accum[24], mask[24], lane[24];
      use_as(fn, &in->arguments[0], cls, input);
      PtxVal dv = {.cls = cls, .is_unsigned = !is_float};
      dv = destination_value(fn, &in->dest, dv);
      reg_name(cls, dv.idx, accum);
      sb_printf(&fn->body, "\tmov.%s %s, %s;\n",
                is_float ? "f32" : "u32", accum, input);
      int mask_index = new_reg(fn, PC_B32);
      int lane_index = new_reg(fn, PC_B32);
      reg_name(PC_B32, mask_index, mask);
      reg_name(PC_B32, lane_index, lane);
      sb_printf(&fn->body, "\tactivemask.b32 %s;\n", mask);
      sb_printf(&fn->body, "\tmov.u32 %s, %%laneid;\n", lane);

      static const unsigned scan_offsets[] = {1, 2, 4, 8, 16};
      for (size_t oi = 0;
           oi < sizeof(scan_offsets) / sizeof(scan_offsets[0]); oi++) {
        unsigned offset = scan_offsets[oi];
        char other[24], source[24], bit[24], active_bits[24];
        char shuffle_ok[24], source_in_range[24], source_active[24], take[24];
        int other_index = new_reg(fn, cls);
        int source_index = new_reg(fn, PC_B32);
        int bit_index = new_reg(fn, PC_B32);
        int active_bits_index = new_reg(fn, PC_B32);
        int shuffle_ok_index = new_reg(fn, PC_PRED);
        int source_in_range_index = new_reg(fn, PC_PRED);
        int source_active_index = new_reg(fn, PC_PRED);
        int take_index = new_reg(fn, PC_PRED);
        reg_name(cls, other_index, other);
        reg_name(PC_B32, source_index, source);
        reg_name(PC_B32, bit_index, bit);
        reg_name(PC_B32, active_bits_index, active_bits);
        reg_name(PC_PRED, shuffle_ok_index, shuffle_ok);
        reg_name(PC_PRED, source_in_range_index, source_in_range);
        reg_name(PC_PRED, source_active_index, source_active);
        reg_name(PC_PRED, take_index, take);
        sb_printf(&fn->body,
                  "\tshfl.sync.up.b32 %s|%s, %s, %u, 0, %s;\n",
                  other, shuffle_ok, accum, offset, mask);
        sb_printf(&fn->body, "\tsetp.ge.u32 %s, %s, %u;\n",
                  source_in_range, lane, offset);
        sb_printf(&fn->body, "\tsub.u32 %s, %s, %u;\n", source, lane,
                  offset);
        sb_printf(&fn->body, "\tmov.u32 %s, 0;\n", bit);
        sb_printf(&fn->body, "\t@%s shl.b32 %s, 1, %s;\n",
                  source_in_range, bit, source);
        sb_printf(&fn->body, "\tand.b32 %s, %s, %s;\n", active_bits,
                  mask, bit);
        sb_printf(&fn->body, "\tsetp.ne.u32 %s, %s, 0;\n", source_active,
                  active_bits);
        sb_printf(&fn->body, "\tand.pred %s, %s, %s;\n", take,
                  shuffle_ok, source_active);
        sb_printf(&fn->body, "\t@%s add.%s %s, %s, %s;\n", take,
                  is_float ? "f32" : "u32", accum, accum, other);
      }

      if (is_exclusive) {
        char other[24], source[24], bit[24], active_bits[24];
        char shuffle_ok[24], has_predecessor[24], source_active[24];
        char take[24], take_active[24];
        int other_index = new_reg(fn, cls);
        int source_index = new_reg(fn, PC_B32);
        int bit_index = new_reg(fn, PC_B32);
        int active_bits_index = new_reg(fn, PC_B32);
        int shuffle_ok_index = new_reg(fn, PC_PRED);
        int has_predecessor_index = new_reg(fn, PC_PRED);
        int source_active_index = new_reg(fn, PC_PRED);
        int take_index = new_reg(fn, PC_PRED);
        int take_active_index = new_reg(fn, PC_PRED);
        reg_name(cls, other_index, other);
        reg_name(PC_B32, source_index, source);
        reg_name(PC_B32, bit_index, bit);
        reg_name(PC_B32, active_bits_index, active_bits);
        reg_name(PC_PRED, shuffle_ok_index, shuffle_ok);
        reg_name(PC_PRED, has_predecessor_index, has_predecessor);
        reg_name(PC_PRED, source_active_index, source_active);
        reg_name(PC_PRED, take_index, take);
        reg_name(PC_PRED, take_active_index, take_active);
        sb_printf(&fn->body,
                  "\tshfl.sync.up.b32 %s|%s, %s, 1, 0, %s;\n",
                  other, shuffle_ok, accum, mask);
        sb_printf(&fn->body, "\tsetp.ge.u32 %s, %s, 1;\n",
                  has_predecessor, lane);
        sb_printf(&fn->body, "\tsub.u32 %s, %s, 1;\n", source, lane);
        sb_printf(&fn->body, "\tmov.u32 %s, 0;\n", bit);
        sb_printf(&fn->body, "\t@%s shl.b32 %s, 1, %s;\n",
                  has_predecessor, bit, source);
        sb_printf(&fn->body, "\tand.b32 %s, %s, %s;\n", active_bits,
                  mask, bit);
        sb_printf(&fn->body, "\tsetp.ne.u32 %s, %s, 0;\n", source_active,
                  active_bits);
        sb_printf(&fn->body, "\tand.pred %s, %s, %s;\n", take,
                  shuffle_ok, has_predecessor);
        sb_printf(&fn->body, "\tand.pred %s, %s, %s;\n", take_active,
                  take, source_active);
        sb_printf(&fn->body, "\tmov.%s %s, %s;\n",
                  is_float ? "f32" : "u32", accum,
                  is_float ? "0f00000000" : "0");
        sb_printf(&fn->body, "\t@%s mov.%s %s, %s;\n", take_active,
                  is_float ? "f32" : "u32", accum, other);
      }
      if (in->dest.name) bind_value(fn, in->dest.name, dv);
    } else {
      ptx_emit_numeric_intrinsic(program, func, fn, in, error, ename, handled);
    }
    return;
  }
  default:
    *handled = 0;
    return;
  }
}

static PtxClass ptx_asm_binding_class(PtxFn *fn, const IRFunction *func,
                                      size_t at, const IRInstruction *in,
                                      size_t k, const char *name) {
  if (in->argument_types && in->argument_types[k]) {
    PtxVal v = descriptor_from_type(in->argument_types[k]);
    if (v.cls != PC_NONE) {
      return v.cls;
    }
  }
  for (size_t j = at + 1; j < func->instruction_count; j++) {
    const IRInstruction *next = &func->instructions[j];
    if (next->op == IR_OP_NOP) {
      continue;
    }
    if (next->op != IR_OP_ASM_RESULT) {
      break;
    }
    if (next->rhs.kind == IR_OPERAND_INT &&
        next->rhs.int_value == (long long)k) {
      PtxVal v = next->value_type ? descriptor_from_type(next->value_type)
                                  : descriptor_from_typename(next->text);
      if (v.cls != PC_NONE) {
        return v.cls;
      }
    }
  }
  const IROperand *argument =
      k < in->argument_count ? &in->arguments[k] : NULL;
  if (argument && (argument->kind == IR_OPERAND_TEMP ||
                   argument->kind == IR_OPERAND_SYMBOL)) {
    PtxClass cls = operand_desc(fn, argument).cls;
    if (cls != PC_NONE) {
      return cls;
    }
  }
  PtxBinding *b = find_binding(fn, name);
  if (b && !b->val.mem_local && !b->val.mem_aggregate &&
      b->val.cls != PC_NONE) {
    return b->val.cls;
  }
  if (argument && argument->kind != IR_OPERAND_NONE) {
    return operand_desc(fn, argument).cls;
  }
  return PC_NONE;
}

static int ptx_asm_reserve(PtxFn *fn, size_t count) {
  if (count <= fn->asm_out_capacity) {
    return 1;
  }
  PtxClass *cls = realloc(fn->asm_out_cls, count * sizeof(*cls));
  if (!cls) {
    return 0;
  }
  fn->asm_out_cls = cls;
  int *idx = realloc(fn->asm_out_idx, count * sizeof(*idx));
  if (!idx) {
    return 0;
  }
  fn->asm_out_idx = idx;
  fn->asm_out_capacity = count;
  return 1;
}

static void ptx_emit_asm_text(PtxFn *fn, const char *text,
                              const IRAsmBinding *bindings, size_t count,
                              char (*regs)[24]) {
  Sb line = {0};
  int line_has_text = 0;
  for (const char *c = text;; c++) {
    if (*c == '\n' || *c == '\0') {
      if (line_has_text) {
        sb_puts(&fn->body, "\t");
        sb_puts(&fn->body, line.data ? line.data : "");
        sb_puts(&fn->body, "\n");
      }
      free(line.data);
      memset(&line, 0, sizeof(line));
      line_has_text = 0;
      if (*c == '\0') {
        break;
      }
      continue;
    }
    if (!line_has_text && (*c == ' ' || *c == '\t' || *c == '\r')) {
      continue;
    }
    line_has_text = 1;
    if (*c == '{') {
      const char *q = c + 1;
      while (*q == ' ' || *q == '\t') {
        q++;
      }
      const char *name0 = q;
      if ((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z') || *q == '_') {
        while ((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z') ||
               (*q >= '0' && *q <= '9') || *q == '_') {
          q++;
        }
        const char *name1 = q;
        while (*q == ' ' || *q == '\t') {
          q++;
        }
        if (*q == '}') {
          char name[128];
          size_t n = (size_t)(name1 - name0);
          if (n >= sizeof(name)) {
            n = sizeof(name) - 1;
          }
          memcpy(name, name0, n);
          name[n] = '\0';
          size_t k = 0;
          while (k < count && strcmp(bindings[k].name, name) != 0) {
            k++;
          }
          if (k < count) {
            sb_puts(&line, regs[k]);
          } else {
            fn_error(fn, "PTX asm: `{%s}` was not bound to an operand", name);
          }
          c = q;
          continue;
        }
      }
    }
    char one[2] = {*c, '\0'};
    sb_puts(&line, one);
  }
  free(line.data);
}

static void ptx_emit_inline_asm(PtxFn *fn, const IRFunction *func,
                                const IRInstruction *in, size_t at) {
  IRAsmBinding *bindings = NULL;
  size_t count = 0;
  fn->asm_out_count = 0;
  if (!in->asm_operands) {
    fn_error(fn, "PTX asm: a block in '%s' was not bound to its operands",
             func->name ? func->name : "?");
    return;
  }
  if (!ir_inline_asm_bindings(in->text, &bindings, &count) ||
      count != in->argument_count || !ptx_asm_reserve(fn, count)) {
    fn_error(fn, "PTX asm: a block in '%s' lost its operand bindings",
             func->name ? func->name : "?");
    free(bindings);
    return;
  }
  char(*regs)[24] = count ? calloc(count, sizeof(*regs)) : NULL;
  if (count && !regs) {
    fn_error(fn, "PTX asm: out of memory");
    free(bindings);
    return;
  }
  for (size_t k = 0; k < count; k++) {
    PtxClass cls =
        ptx_asm_binding_class(fn, func, at, in, k, bindings[k].name);
    if (cls == PC_NONE) {
      fn_error(fn, "PTX asm: `{%s}` is not a scalar held in a register "
                   "(an aggregate, or a local whose address is taken)",
               bindings[k].name);
      break;
    }
    fn->asm_out_cls[k] = cls;
    fn->asm_out_idx[k] = -1;
    if (bindings[k].writes) {
      int idx = new_reg(fn, cls);
      reg_name(cls, idx, regs[k]);
      fn->asm_out_idx[k] = idx;
      if (bindings[k].reads) {
        char src[24];
        use_as(fn, &in->arguments[k], cls, src);
        sb_printf(&fn->body, "\tmov%s %s, %s;\n", cls_regtype(cls), regs[k],
                  src);
      }
    } else {
      use_as(fn, &in->arguments[k], cls, regs[k]);
    }
  }
  fn->asm_out_count = count;
  if (!fn->error) {
    ptx_emit_asm_text(fn, in->text ? in->text : "", bindings, count, regs);
  }
  free(regs);
  free(bindings);
}

static void ptx_emit_asm_result(PtxFn *fn, const IRInstruction *in) {
  long long k = in->rhs.kind == IR_OPERAND_INT ? in->rhs.int_value : -1;
  if (k < 0 || (size_t)k >= fn->asm_out_count || fn->asm_out_idx[k] < 0 ||
      !in->dest.name) {
    fn_error(fn, "PTX asm: a result does not follow the block that writes it");
    return;
  }
  PtxClass cls = fn->asm_out_cls[k];
  char out[24];
  reg_name(cls, fn->asm_out_idx[k], out);
  PtxBinding *db = find_binding(fn, in->dest.name);
  PtxClass dc = (db && db->val.cls != PC_NONE) ? db->val.cls : cls;
  char src[24];
  coerce(fn, cls, in->is_unsigned, out, dc, src);
  PtxVal dv;
  if (db && !db->val.mem_local) {
    dv = db->val;
  } else {
    dv = (PtxVal){0};
    dv.cls = dc;
    dv.is_unsigned = db ? db->val.is_unsigned : in->is_unsigned;
    dv.idx = new_reg(fn, dc);
  }
  char dn[24];
  reg_name(dv.cls, dv.idx, dn);
  if (strcmp(dn, src) != 0) {
    sb_printf(&fn->body, "\tmov%s %s, %s;\n", cls_regtype(dv.cls), dn, src);
  }
  bind_value(fn, in->dest.name, dv);
}

static int ptx_guard_name_used_outside(const IRFunction *func,
                                       const IROperand *dest, size_t first,
                                       size_t last) {
  for (size_t i = 0; i < func->instruction_count; i++) {
    if (i >= first && i < last) {
      continue;
    }
    const IRInstruction *in = &func->instructions[i];
    const IROperand *operands[3] = {&in->dest, &in->lhs, &in->rhs};
    for (int k = 0; k < 3; k++) {
      if (operands[k]->kind == dest->kind && operands[k]->name &&
          strcmp(operands[k]->name, dest->name) == 0) {
        return 1;
      }
    }
    for (size_t a = 0; a < in->argument_count; a++) {
      if (in->arguments[a].kind == dest->kind && in->arguments[a].name &&
          strcmp(in->arguments[a].name, dest->name) == 0) {
        return 1;
      }
    }
  }
  return 0;
}

static int ptx_label_run_contains(const IRFunction *func, size_t at,
                                  const char *label) {
  for (size_t i = at; i < func->instruction_count; i++) {
    const IRInstruction *in = &func->instructions[i];
    if (in->op == IR_OP_NOP) {
      continue;
    }
    if (in->op != IR_OP_LABEL) {
      return 0;
    }
    if (in->text && strcmp(in->text, label) == 0) {
      return 1;
    }
  }
  return 0;
}

static int ptx_branch_skips_only_async_copies(const IRFunction *func,
                                              size_t at, size_t *end,
                                              size_t *jump) {
  const IRInstruction *branch = &func->instructions[at];
  const char *target = branch->text;
  size_t copies = 0;
  if (!target) {
    return 0;
  }
  *jump = (size_t)-1;
  for (size_t i = at + 1; i < func->instruction_count; i++) {
    const IRInstruction *in = &func->instructions[i];
    switch (in->op) {
    case IR_OP_NOP:
    case IR_OP_DECLARE_LOCAL:
      continue;
    case IR_OP_ASYNC_COPY:
      copies++;
      continue;
    case IR_OP_BINARY:
    case IR_OP_UNARY:
    case IR_OP_CAST:
    case IR_OP_ASSIGN:
      if (in->is_volatile || !in->dest.name ||
          (in->dest.kind != IR_OPERAND_TEMP &&
           in->dest.kind != IR_OPERAND_SYMBOL)) {
        return 0;
      }
      continue;
    case IR_OP_JUMP: {
      size_t next = i + 1;
      while (next < func->instruction_count &&
             func->instructions[next].op == IR_OP_NOP) {
        next++;
      }
      if (!in->text || !ptx_label_run_contains(func, next, target) ||
          !ptx_label_run_contains(func, next, in->text)) {
        return 0;
      }
      *jump = i;
      *end = next;
      break;
    }
    case IR_OP_LABEL:
      if (!ptx_label_run_contains(func, i, target)) {
        return 0;
      }
      *end = i;
      break;
    default:
      return 0;
    }
    break;
  }
  if (copies == 0 || *end <= at) {
    return 0;
  }
  for (size_t i = at + 1; i < *end; i++) {
    const IRInstruction *in = &func->instructions[i];
    if ((in->op == IR_OP_BINARY || in->op == IR_OP_UNARY ||
         in->op == IR_OP_CAST || in->op == IR_OP_ASSIGN) &&
        ptx_guard_name_used_outside(func, &in->dest, at + 1, *end)) {
      return 0;
    }
  }
  return 1;
}

static void ptx_emit_result(IRProgram *program, IRFunction *func, PtxFn *fn,
                        const IRInstruction *in, size_t *ii, char **error,
                        int target_arch, int returns_void, const char *ename,
                        int *handled) {
  (void)ii;
  (void)handled;
  (void)target_arch;
  (void)program;
  (void)error;
  switch (in->op) {
  case IR_OP_RETURN:
    if (func->is_kernel || returns_void) {
      if (in->lhs.kind != IR_OPERAND_NONE) {
        fn_error(fn, "PTX: void device function '%s' returns a value",
                 func->name ? func->name : "?");
      } else {
        sb_puts(&fn->body, "\tret;\n");
      }
    } else if (in->lhs.kind == IR_OPERAND_NONE) {
      fn_error(fn, "PTX: non-void device function '%s' has an empty return",
               func->name ? func->name : "?");
    } else if (fn->return_desc.mem_aggregate) {
      PtxBinding *value = named_binding(fn, &in->lhs);
      if (!value || !value->val.mem_aggregate) {
        fn_error(fn, "PTX: device function '%s' returns a record it has no "
                      "storage for",
                 func->name ? func->name : "?");
        break;
      }
      char source[24], destination[512];
      reg_name(PC_B64, value->val.mem_addr, source);
      snprintf(destination, sizeof(destination), "%s_ret", ename);
      ptx_block_copy(fn, ".param", destination, ".local", source,
                     fn->return_desc.mem_size, fn->return_desc.mem_align);
      sb_puts(&fn->body, "\tret;\n");
    } else {
      char value[24];
      use_as(fn, &in->lhs, fn->return_desc.cls, value);
      sb_printf(&fn->body, "\tst.param.%s [%s_ret], %s;\n\tret;\n",
                device_param_storage_type(fn->return_desc), ename, value);
    }
    break;
  case IR_OP_ADDRESS_OF: {
    PtxBinding *home = named_binding(fn, &in->lhs);
    if (!home || !home->val.mem_local) {
      fn_error(fn, "PTX: cannot take the address of '%s' in device code",
               in->lhs.name ? in->lhs.name : "?");
      break;
    }
    PtxVal a = {0};
    a.cls = PC_B64;
    a.idx = home->val.mem_addr;
    a.is_ptr = 1;
    a.is_unsigned = 1;
    a.elem = home->val.mem_aggregate ? MTLC_TYPE_VOID : home->val.elem;
    a.ptr_align = home->val.mem_aggregate ? home->val.mem_align : 0;
    a.address_space = MTLC_ADDRESS_SPACE_PRIVATE;
    if (in->dest.name) {
      bind_value(fn, in->dest.name, a);
    }
    break;
  }
  case IR_OP_INLINE_ASM:
    ptx_emit_inline_asm(fn, func, in, *ii);
    break;
  case IR_OP_ASM_RESULT:
    ptx_emit_asm_result(fn, in);
    break;
  default:
    fn_error(fn, "PTX: unsupported IR opcode %d in device function '%s'", in->op,
             func->name ? func->name : "?");
    break;
  }
}

typedef void (*PtxHandler)(IRProgram *program, IRFunction *func, PtxFn *fn,
                           const IRInstruction *in, size_t *ii, char **error,
                           int target_arch, int returns_void,
                           const char *ename, int *handled);

static const PtxHandler PTX_HANDLERS[] = {
    ptx_emit_device, ptx_emit_control, ptx_emit_assign, ptx_emit_memory,
    ptx_emit_arith,  ptx_emit_call,    ptx_emit_result,
};

typedef struct {
  IRProgram *program;
  IRFunction *func;
  PtxFn fn;
  const IRModuleSymbol *symbol;
  int returns_void;
  char ename[256];
  Sb sig;
  PtxVal *param_descs;
  size_t dynamic_alignment;
  char dynamic_storage[512];
} PtxEmit;

static const char *ptx_emit_name(const PtxEmit *e) {
  return e->func->name ? e->func->name : "?";
}

static PtxVal ptx_aggregate_descriptor(const MtlcType *type) {
  PtxVal d = {0};
  d.cls = PC_B64;
  d.elem = MTLC_TYPE_VOID;
  d.mem_local = 1;
  d.mem_aggregate = 1;
  d.mem_size = mtlc_type_size(type);
  d.mem_align = mtlc_type_alignment(type);
  if (!d.mem_align) {
    d.mem_align = 1;
  }
  return d;
}

static void ptx_emit_return_descriptor(PtxEmit *e) {
  const MtlcType *return_type =
      ptx_function_return_type(e->program, e->func, e->symbol);
  e->returns_void = ptx_type_is_void(
      return_type, e->func ? e->func->return_type_name : NULL);
  e->fn.return_desc =
      e->returns_void
          ? (PtxVal){0}
          : (return_type ? descriptor_from_type(return_type)
                         : descriptor_from_typename(e->func->return_type_name));
  if (!e->returns_void && ptx_type_is_aggregate(return_type)) {
    e->fn.return_desc = ptx_aggregate_descriptor(return_type);
  }
}

static void ptx_emit_signature_head(PtxEmit *e) {
  if (e->func->is_kernel) {
    sb_printf(&e->sig, ".visible .entry %s(", e->ename);
  } else if (e->returns_void) {
    sb_printf(&e->sig, ".func %s(", e->ename);
  } else if (e->fn.return_desc.mem_aggregate) {
    sb_printf(&e->sig, ".func (.param .align %zu .b8 %s_ret[%zu]) %s(",
              e->fn.return_desc.mem_align, e->ename,
              e->fn.return_desc.mem_size, e->ename);
  } else {
    sb_printf(&e->sig, ".func (.param .%s %s_ret) %s(",
              device_param_storage_type(e->fn.return_desc), e->ename,
              e->ename);
  }
}

static const MtlcType *ptx_parameter_type(const PtxEmit *e, size_t p) {
  return e->symbol && e->symbol->kind == IR_MODSYM_FUNCTION &&
                 p < e->symbol->param_count
             ? e->symbol->param_types[p]
             : NULL;
}

static PtxVal ptx_parameter_descriptor(PtxEmit *e, size_t p,
                                       const MtlcType *pt) {
  const char *tn = e->func->parameter_types ? e->func->parameter_types[p]
                                            : NULL;
  PtxVal d = pt ? descriptor_from_type(pt) : descriptor_from_typename(tn);
  if (!e->func->is_kernel && d.is_ptr &&
      (!pt || pt->address_space == MTLC_ADDRESS_SPACE_DEFAULT)) {
    d.address_space = MTLC_ADDRESS_SPACE_GENERIC;
  }
  if (ptx_type_is_aggregate(pt)) {
    d = ptx_aggregate_descriptor(pt);
    if (!d.mem_size) {
      fn_error(&e->fn, "PTX: parameter %zu of '%s' has an empty record type",
               p, ptx_emit_name(e));
    }
  }
  return d;
}

static void ptx_signature_parameter(PtxEmit *e, size_t p, PtxVal d,
                                    const MtlcType *pt) {
  if (p) {
    sb_puts(&e->sig, ",");
  }
  if (d.mem_aggregate) {
    sb_printf(&e->sig, "\n    .param .align %zu .b8 %s_p%zu[%zu]", d.mem_align,
              e->ename, p, d.mem_size);
    return;
  }
  if (e->func->is_kernel && d.is_ptr) {
    const char *space = ptx_memory_space(d.address_space);
    size_t alignment = pt && pt->pointee_align ? pt->pointee_align
                       : pt && pt->base_type && pt->base_type->alignment
                           ? pt->base_type->alignment
                           : 4;
    if (!space) {
      fn_error(&e->fn, "PTX: invalid address space %d on parameter %zu",
               (int)d.address_space, p);
    } else {
      sb_printf(&e->sig, "\n    .param .%s .ptr%s.align %zu %s_p%zu",
                param_storage_type(d), space, alignment, e->ename, p);
    }
    return;
  }
  sb_printf(&e->sig, "\n    .param .%s %s_p%zu",
            e->func->is_kernel ? param_storage_type(d)
                               : device_param_storage_type(d),
            e->ename, p);
}

static void ptx_emit_signature(PtxEmit *e) {
  IRFunction *func = e->func;
  ptx_emit_signature_head(e);
  e->param_descs = calloc(func->parameter_count + 1, sizeof(PtxVal));
  for (size_t p = 0; p < func->parameter_count; p++) {
    const MtlcType *pt = ptx_parameter_type(e, p);
    PtxVal d = ptx_parameter_descriptor(e, p, pt);
    e->param_descs[p] = d;
    ptx_signature_parameter(e, p, d, pt);
  }
  sb_puts(&e->sig, "\n)\n");
  if (func->is_kernel && func->kernel_block[0] > 0) {
    sb_printf(&e->sig, ".reqntid %d, %d, %d\n", func->kernel_block[0],
              func->kernel_block[1] > 0 ? func->kernel_block[1] : 1,
              func->kernel_block[2] > 0 ? func->kernel_block[2] : 1);
  }
}

static void ptx_emit_dynamic_workgroup(PtxEmit *e) {
  IRFunction *func = e->func;
  for (size_t i = 0; i < func->instruction_count && !e->fn.error; i++) {
    const IRInstruction *in = &func->instructions[i];
    size_t alignment = 0;
    if (in->op != IR_OP_ADDRESS_SPACE_ALLOC ||
        in->rhs.kind != IR_OPERAND_INT || in->rhs.int_value != 0) {
      continue;
    }
    if (!func->is_kernel || !in->dest.name || !in->value_type ||
        in->value_type->kind != MTLC_TYPE_POINTER ||
        !in->value_type->base_type ||
        in->address_space != MTLC_ADDRESS_SPACE_WORKGROUP ||
        in->value_type->address_space != MTLC_ADDRESS_SPACE_WORKGROUP ||
        mtlc_type_size(in->value_type->base_type) == 0) {
      fn_error(&e->fn, "PTX: invalid dynamic workgroup view in '%s'",
               ptx_emit_name(e));
      break;
    }
    alignment = mtlc_type_alignment(in->value_type->base_type);
    if (alignment < 32) alignment = 32;
    if (alignment > e->dynamic_alignment) {
      e->dynamic_alignment = alignment;
    }
  }
  if (e->dynamic_alignment && !e->fn.error) {
    char raw[512];
    snprintf(raw, sizeof(raw), "%s_dynamic_workgroup_storage", e->ename);
    sanitize_into(raw, e->dynamic_storage, sizeof(e->dynamic_storage));
  }
}

static int ptx_allocation_is_valid(PtxEmit *e, const IRInstruction *in,
                                   int is_dynamic) {
  if (!e->func->is_kernel || !in->dest.name || !in->value_type ||
      in->value_type->kind != MTLC_TYPE_POINTER ||
      !in->value_type->base_type || in->rhs.kind != IR_OPERAND_INT ||
      in->rhs.int_value < 0 ||
      (in->address_space != MTLC_ADDRESS_SPACE_WORKGROUP &&
       in->address_space != MTLC_ADDRESS_SPACE_PRIVATE) ||
      in->value_type->address_space != in->address_space ||
      (is_dynamic && in->address_space != MTLC_ADDRESS_SPACE_WORKGROUP)) {
    fn_error(&e->fn, "PTX: invalid address-space allocation in '%s'",
             ptx_emit_name(e));
    return 0;
  }
  return 1;
}

static void ptx_mark_line(PtxFn *fn, size_t line) {
  if (!fn->reported) return;
  if (fn->line_mark_count >= 2 &&
      fn->line_marks[fn->line_mark_count - 1] == line)
    return;
  if (fn->line_mark_count + 2 > fn->line_mark_capacity) {
    size_t capacity = fn->line_mark_capacity ? fn->line_mark_capacity * 2 : 64;
    size_t *grown = realloc(fn->line_marks, capacity * sizeof(*grown));
    if (!grown) return;
    fn->line_marks = grown;
    fn->line_mark_capacity = capacity;
  }
  fn->line_marks[fn->line_mark_count++] = fn->body.len;
  fn->line_marks[fn->line_mark_count++] = line;
}

static void ptx_report_memory_value(PtxEmit *e, const IRInstruction *in,
                                    const char *what, int aggregate,
                                    size_t bytes) {
  char shown[128], subject[176], headline[96], cause[300], reason[380];
  int has_reason = 0;
  if (!e->fn.reported) return;
  ir_display_name(in->dest.name, shown, sizeof(shown));
  snprintf(subject, sizeof(subject), "%s `%s`", what, shown);
  snprintf(headline, sizeof(headline), "emitted as local memory, %zu bytes",
           bytes);
  has_reason = ir_device_report_storage_reason(e->func, in->dest.name,
                                               aggregate, cause, sizeof(cause));
  snprintf(reason, sizeof(reason), "%s, so %s", cause,
           aggregate ? "its elements cannot each take a register"
                     : "it needs an address in memory");
  ir_explain_device_add(e->func->name, IR_DEVICE_VALUES_IN_MEMORY, 1);
  ir_explain_device_add(e->func->name, IR_DEVICE_MEMORY_BYTES,
                        (long long)bytes);
  ir_explain_device_note(e->func->name, in->location.line, "storage", subject,
                         has_reason ? IR_DEVICE_MISS : IR_DEVICE_FACT, headline,
                         has_reason ? reason : NULL, NULL, "local-memory");
  ir_explain_device_note_quantity("bytes", (long long)bytes);
}

static void ptx_report_allocation(PtxEmit *e, const IRInstruction *in,
                                  int is_dynamic, size_t bytes) {
  if (!e->fn.reported) return;
  if (in->address_space != MTLC_ADDRESS_SPACE_WORKGROUP) {
    ptx_report_memory_value(e, in, "private array", 1, bytes);
    return;
  }
  if (is_dynamic) {
    ir_explain_device_set(e->func->name, IR_DEVICE_WORKGROUP_LAUNCH_SIZED, 1);
    return;
  }
  ir_explain_device_add(e->func->name, IR_DEVICE_WORKGROUP_BYTES,
                        (long long)bytes);
}

static void ptx_emit_allocations(PtxEmit *e) {
  IRFunction *func = e->func;
  for (size_t i = 0; i < func->instruction_count && !e->fn.error; i++) {
    const IRInstruction *in = &func->instructions[i];
    size_t elem_size = 0, alignment = 0, count = 0;
    PtxVal pointer;
    int is_dynamic = 0;
    if (in->op != IR_OP_ADDRESS_SPACE_ALLOC) continue;
    if (ptx_tile_is_type(in->value_type)) {
      ptx_tile_register(&e->fn, in);
      continue;
    }
    is_dynamic = in->rhs.kind == IR_OPERAND_INT && in->rhs.int_value == 0;
    if (!ptx_allocation_is_valid(e, in, is_dynamic)) break;
    elem_size = mtlc_type_size(in->value_type->base_type);
    alignment = mtlc_type_alignment(in->value_type->base_type);
    if (in->address_space == MTLC_ADDRESS_SPACE_WORKGROUP && alignment < 32)
      alignment = 32;
    count = (size_t)in->rhs.int_value;
    if (!elem_size || (!is_dynamic && count > SIZE_MAX / elem_size)) {
      fn_error(&e->fn, "PTX: address-space allocation '%s' overflows",
               in->dest.name);
      break;
    }
    if (!is_dynamic) {
      char raw[512], storage[512];
      snprintf(raw, sizeof(raw), "%s_%s_storage", e->ename, in->dest.name);
      sanitize_into(raw, storage, sizeof(storage));
      sb_printf(&e->fn.body, "\t%s .align %zu .b8 %s[%zu];\n",
                in->address_space == MTLC_ADDRESS_SPACE_WORKGROUP ? ".shared"
                                                                  : ".local",
                alignment ? alignment : 1, storage, elem_size * count);
    }
    ptx_report_allocation(e, in, is_dynamic, elem_size * count);
    pointer = descriptor_from_type(in->value_type);
    pointer.idx = new_reg(&e->fn, PC_B64);
    bind_value(&e->fn, in->dest.name, pointer);
  }
}

static void ptx_emit_transfer_barrier(PtxEmit *e) {
  IRFunction *func = e->func;
  char barrier_name[512];
  for (size_t i = 0; i < func->instruction_count; i++) {
    const IRInstruction *in = &func->instructions[i];
    if (in->op != IR_OP_TENSOR_TRANSFER ||
        IR_TENSOR_TRANSFER(in).direction !=
            MTLC_TENSOR_TRANSFER_GLOBAL_TO_WORKGROUP ||
        !ptx_tensor_transfer_native_capable(
            &e->fn, &IR_TENSOR_TRANSFER(in),
            in->tensor_transfer_has_prepared_view)) {
      continue;
    }
    ptx_tensor_transfer_barrier_name(&e->fn, barrier_name,
                                     sizeof(barrier_name));
    sb_printf(&e->fn.body, "\t.shared .align 8 .b8 %s[8];\n", barrier_name);
    return;
  }
}

static void ptx_emit_aggregate_parameter(PtxEmit *e, size_t p, PtxVal d) {
  char raw[512], storage[512], pointer[24], source[512];
  snprintf(raw, sizeof(raw), "%s_p%zu_local", e->ename, p);
  sanitize_into(raw, storage, sizeof(storage));
  sb_printf(&e->fn.body, "\t.local .align %zu .b8 %s[%zu];\n", d.mem_align,
            storage, d.mem_size);
  d.mem_addr = new_reg(&e->fn, PC_B64);
  reg_name(PC_B64, d.mem_addr, pointer);
  sb_printf(&e->fn.body, "\tmov.u64 %s, %s;\n", pointer, storage);
  snprintf(source, sizeof(source), "%s_p%zu", e->ename, p);
  ptx_block_copy(&e->fn, ".local", pointer, ".param", source, d.mem_size,
                 d.mem_align);
  if (e->func->parameter_names && e->func->parameter_names[p]) {
    bind_value(&e->fn, e->func->parameter_names[p], d);
  }
}

static void ptx_emit_scalar_parameter(PtxEmit *e, size_t p, PtxVal d) {
  char rn[24];
  d.idx = new_reg(&e->fn, d.cls);
  reg_name(d.cls, d.idx, rn);
  if (!d.is_ptr &&
      (d.elem == MTLC_TYPE_FLOAT16 || d.elem == MTLC_TYPE_BFLOAT16)) {
    char tmp[24];
    reg_name(PC_B16, new_reg(&e->fn, PC_B16), tmp);
    sb_printf(&e->fn.body, "\tld.param.b16 %s, [%s_p%zu];\n", tmp, e->ename, p);
    sb_printf(&e->fn.body, "\tcvt.f32.%s %s, %s;\n",
              d.elem == MTLC_TYPE_FLOAT16 ? "f16" : "bf16", rn, tmp);
  } else {
    sb_printf(&e->fn.body, "\tld.param.%s %s, [%s_p%zu];\n",
              e->func->is_kernel ? param_storage_type(d)
                                 : device_param_storage_type(d),
              rn, e->ename, p);
  }
  if (e->func->parameter_names && e->func->parameter_names[p]) {
    bind_value(&e->fn, e->func->parameter_names[p], d);
  }
}

static void ptx_emit_parameter_loads(PtxEmit *e) {
  for (size_t p = 0; p < e->func->parameter_count && !e->fn.error; p++) {
    PtxVal d = e->param_descs[p];
    if (d.mem_aggregate) {
      ptx_emit_aggregate_parameter(e, p, d);
    } else {
      ptx_emit_scalar_parameter(e, p, d);
    }
    if (e->fn.reported && d.mem_aggregate) {
      ir_explain_device_add(e->func->name, IR_DEVICE_VALUES_IN_MEMORY, 1);
      ir_explain_device_add(e->func->name, IR_DEVICE_MEMORY_BYTES,
                            (long long)d.mem_size);
    } else if (e->fn.reported) {
      ir_explain_device_add(e->func->name, IR_DEVICE_VALUES_IN_REGISTERS, 1);
    }
  }
}

static int ptx_local_declared_before(const IRFunction *func, size_t index) {
  const char *name = func->instructions[index].dest.name;
  for (size_t i = 0; i < index; i++) {
    const IRInstruction *in = &func->instructions[i];
    if (in->op == IR_OP_DECLARE_LOCAL && in->dest.name &&
        strcmp(in->dest.name, name) == 0)
      return 1;
  }
  return 0;
}

static void ptx_emit_allocation_pointers(PtxEmit *e) {
  IRFunction *func = e->func;
  for (size_t i = 0; i < func->instruction_count && !e->fn.error; i++) {
    const IRInstruction *in = &func->instructions[i];
    PtxBinding *binding = NULL;
    char raw[512], storage[512], pointer[24];
    if (in->op != IR_OP_ADDRESS_SPACE_ALLOC || !in->dest.name) continue;
    if (ptx_tile_is_type(in->value_type)) continue;
    binding = find_binding(&e->fn, in->dest.name);
    if (in->rhs.kind == IR_OPERAND_INT && in->rhs.int_value == 0) {
      snprintf(storage, sizeof(storage), "%s", e->dynamic_storage);
    } else {
      snprintf(raw, sizeof(raw), "%s_%s_storage", e->ename, in->dest.name);
      sanitize_into(raw, storage, sizeof(storage));
    }
    if (!binding) {
      fn_error(&e->fn, "PTX: allocation '%s' has no pointer binding",
               in->dest.name);
      break;
    }
    reg_name(PC_B64, binding->val.idx, pointer);
    sb_printf(&e->fn.body, "\tmov.u64 %s, %s;\n", pointer, storage);
  }
}

static int ptx_local_extent(PtxEmit *e, const IRInstruction *in, int aggregate,
                            size_t *size, size_t *alignment) {
  *size = 0;
  *alignment = 0;
  if (in->value_type) {
    *size = mtlc_type_size(in->value_type);
    *alignment = mtlc_type_alignment(in->value_type);
  }
  if (!*size && !aggregate) {
    PtxVal s = descriptor_from_typename(in->text);
    *size = ptx_class_width(s.cls);
    *alignment = *size;
  }
  if (!*size) {
    fn_error(&e->fn, "PTX: local '%s' has unsupported type '%s'",
             in->dest.name, in->text ? in->text : "?");
    return 0;
  }
  if (!*alignment) *alignment = 1;
  return 1;
}

static void ptx_emit_locals(PtxEmit *e) {
  IRFunction *func = e->func;
  for (size_t i = 0; i < func->instruction_count && !e->fn.error; i++) {
    const IRInstruction *in = &func->instructions[i];
    char raw[512], storage[512], pointer[24];
    size_t size = 0, alignment = 0;
    int aggregate = 0;
    PtxVal v;
    if (in->op != IR_OP_DECLARE_LOCAL || !in->dest.name) continue;
    if (find_binding(&e->fn, in->dest.name)) continue;
    aggregate = ptx_type_is_aggregate(in->value_type);
    if (!aggregate && !ptx_local_address_taken(func, in->dest.name)) {
      if (e->fn.reported && !ptx_local_declared_before(func, i))
        ir_explain_device_add(func->name, IR_DEVICE_VALUES_IN_REGISTERS, 1);
      continue;
    }
    if (!ptx_local_extent(e, in, aggregate, &size, &alignment)) break;
    ptx_report_memory_value(
        e, in,
        !aggregate ? "local"
                   : in->value_type->kind == MTLC_TYPE_ARRAY ? "array"
                                                            : "record",
        aggregate, size);
    snprintf(raw, sizeof(raw), "%s_%s_local", e->ename, in->dest.name);
    sanitize_into(raw, storage, sizeof(storage));
    sb_printf(&e->fn.body, "\t.local .align %zu .b8 %s[%zu];\n", alignment,
              storage, size);
    v = aggregate ? (PtxVal){0}
                  : (in->value_type ? descriptor_from_type(in->value_type)
                                    : descriptor_from_typename(in->text));
    v.mem_local = 1;
    v.mem_aggregate = aggregate;
    v.mem_addr = new_reg(&e->fn, PC_B64);
    v.mem_size = size;
    v.mem_align = alignment;
    if (aggregate) {
      v.cls = PC_B64;
      v.elem = MTLC_TYPE_VOID;
    }
    reg_name(PC_B64, v.mem_addr, pointer);
    sb_printf(&e->fn.body, "\tmov.u64 %s, %s;\n", pointer, storage);
    bind_value(&e->fn, in->dest.name, v);
  }
}

static void ptx_emit_vector_load(PtxEmit *e, size_t ii,
                                 const PtxVectorPlan *plan) {
  IRFunction *func = e->func;
  const IRInstruction *in = &func->instructions[ii];
  unsigned char width = plan[ii].width;
  const IROperand *base = NULL;
  const MtlcType *pointer_type = NULL;
  long long offset = 0;
  char addrreg[24];
  char names[4][24];
  char list[128];
  size_t used = 0;
  int is_unsigned = 0;
  PtxClass cls;
  MtlcTypeKind elem;
  const char *space;

  ptx_address_parts(&e->fn, func, ii, &in->lhs, &base, &offset);
  pointer_type = ptx_load_pointer_type(e->program, func, base, ii);
  elem = pointer_type && pointer_type->base_type ? pointer_type->base_type->kind
                                                 : MTLC_TYPE_VOID;
  cls = elem_class(elem, &is_unsigned);
  space = ptx_load_space(pointer_type ? pointer_type->address_space
                                      : MTLC_ADDRESS_SPACE_GENERIC);
  use_as(&e->fn, base, PC_B64, addrreg);
  list[0] = '\0';
  for (unsigned char k = 0; k < width; k++) {
    const IRInstruction *member = &func->instructions[plan[ii].members[k]];
    PtxVal dv = {0};
    dv.cls = cls;
    dv.is_unsigned = is_unsigned;
    dv = destination_value(&e->fn, &member->dest, dv);
    reg_name(cls, dv.idx, names[k]);
    if (member->dest.name) {
      bind_value(&e->fn, member->dest.name, dv);
    }
    used += (size_t)snprintf(list + used, sizeof(list) - used, "%s%s",
                             k ? ", " : "", names[k]);
  }
  sb_printf(&e->fn.body, "\tld%s.v%u.%s {%s}, [%s+%lld];\n", space,
            (unsigned)width, mem_type_suffix(elem), list, addrreg, offset);
  g_ptx_spaced_accesses++;
  g_ptx_vector_groups++;
  g_ptx_vector_loads_saved += width - 1;
  if (e->fn.reported) {
    char subject[200], headline[96], reason[120];
    ptx_report_vector_subject(func, ii, base, "adjacent loads", width, subject,
                              sizeof(subject));
    snprintf(headline, sizeof(headline), "one ld%s.v%u.%s", space,
             (unsigned)width, mem_type_suffix(elem));
    snprintf(reason, sizeof(reason),
             "the pointer's type declares %zu byte alignment",
             pointer_type ? pointer_type->pointee_align : (size_t)0);
    ir_explain_device_add(func->name, IR_DEVICE_ACCESSES_NAMED, 1);
    ir_explain_device_add(func->name, IR_DEVICE_VECTOR_GROUPS, 1);
    ir_explain_device_add(func->name, IR_DEVICE_LOADS_SAVED, width - 1);
    ir_explain_device_note_site(func->name, in->location.line, "vector",
                                subject, IR_DEVICE_GOOD, headline, reason, NULL,
                                "vector-load");
  }
}

static void ptx_emit_instruction(PtxEmit *e, size_t *ii, char **error,
                                 int target_arch) {
  const IRInstruction *in = &e->func->instructions[*ii];
  for (size_t h = 0; h < sizeof(PTX_HANDLERS) / sizeof(PTX_HANDLERS[0]); h++) {
    int handled = 1;
    PTX_HANDLERS[h](e->program, e->func, &e->fn, in, ii, error, target_arch,
                    e->returns_void, e->ename, &handled);
    if (handled) {
      return;
    }
  }
}

static void ptx_emit_body(PtxEmit *e, char **error, int target_arch) {
  clock_t analysis_start = clock();
  PtxVectorPlan *plan = ptx_plan_vector_loads(&e->fn, e->program, e->func);
  g_ptx_analysis_seconds +=
      (double)(clock() - analysis_start) / (double)CLOCKS_PER_SEC;
  for (size_t ii = 0; ii < e->func->instruction_count && !e->fn.error; ii++) {
    const IRInstruction *in = &e->func->instructions[ii];
    if (plan && in->op == IR_OP_LOAD && plan[ii].absorbed) {
      continue;
    }
    ptx_mark_line(&e->fn, ir_device_report_code_line(e->func, in));
    if (plan && in->op == IR_OP_LOAD && plan[ii].width) {
      ptx_emit_vector_load(e, ii, plan);
      continue;
    }
    if (e->fn.async_guard[0] && ii >= e->fn.async_guard_end) {
      e->fn.async_guard[0] = '\0';
    }
    if (e->fn.async_guard[0] && ii == e->fn.async_guard_jump) {
      continue;
    }
    ptx_emit_instruction(e, &ii, error, target_arch);
  }
  free(plan);
}

static void ptx_report_code(PtxEmit *e) {
  const char *text = e->fn.body.data;
  size_t mark = 0;
  size_t line = e->func->location.line;
  if (!e->fn.reported || !text) return;
  for (size_t at = 0; text[at];) {
    size_t end = at;
    size_t start = at;
    size_t length = 0;
    char row[512];
    while (text[end] && text[end] != '\n') end++;
    while (mark < e->fn.line_mark_count && e->fn.line_marks[mark] <= at) {
      line = e->fn.line_marks[mark + 1];
      mark += 2;
    }
    while (start < end && (text[start] == '\t' || text[start] == ' ')) start++;
    length = end - start;
    if (length >= sizeof(row)) length = sizeof(row) - 1;
    memcpy(row, text + start, length);
    row[length] = '\0';
    if (length) ir_explain_device_code(e->func->name, line, row);
    at = text[end] ? end + 1 : end;
  }
}

static void ptx_emit_write(PtxEmit *e, FILE *out) {
  static const PtxClass classes[6] = {PC_PRED, PC_B16, PC_B32,
                                      PC_B64,  PC_F32, PC_F64};
  ptx_report_code(e);
  fputs(e->sig.data, out);
  fputs("{\n", out);
  for (int c = 0; c < 6; c++) {
    PtxClass cc = classes[c];
    if (e->fn.count[cc] > 0) {
      fprintf(out, "\t.reg %s %s<%d>;\n", cls_regtype(cc), cls_prefix(cc),
              e->fn.count[cc]);
    }
  }
  fputs(e->fn.declarations.data ? e->fn.declarations.data : "", out);
  fputs(e->fn.body.data ? e->fn.body.data : "", out);
  fputs(e->func->is_kernel ? "\tret;\n}\n\n" : "}\n\n", out);
}

static void ptx_emit_release(PtxEmit *e) {
  free(e->sig.data);
  free(e->fn.body.data);
  free(e->fn.declarations.data);
  free(e->param_descs);
  for (size_t i = 0; i < e->fn.nbinds; i++) {
    free(e->fn.binds[i].name);
  }
  free(e->fn.binds);
  free(e->fn.def_counts);
  free(e->fn.tensor_residencies);
  for (size_t i = 0; i < e->fn.tile_count; i++) {
    free(e->fn.tiles[i].name);
  }
  free(e->fn.tiles);
  free(e->fn.tile_role);
  free(e->fn.tile_window_start);
  free(e->fn.asm_out_cls);
  free(e->fn.asm_out_idx);
  free(e->fn.generic_names);
  free(e->fn.line_marks);
}

static void ptx_count_one_definition(PtxFn *fn, const char *name) {
  PtxDefCount *slot = ptx_def_slot(fn, name);
  if (!slot) {
    return;
  }
  slot->name = name;
  slot->count++;
}

static void ptx_count_definitions(PtxFn *fn, const IRFunction *func) {
  size_t needed = (func->instruction_count + func->parameter_count + 1) * 2;
  size_t capacity = 16;
  while (capacity < needed) {
    capacity *= 2;
  }
  fn->def_counts = calloc(capacity, sizeof(PtxDefCount));
  if (!fn->def_counts) {
    return;
  }
  fn->def_capacity = capacity;
  for (size_t p = 0; p < func->parameter_count; p++) {
    if (func->parameter_names && func->parameter_names[p]) {
      ptx_count_one_definition(fn, func->parameter_names[p]);
    }
  }
  for (size_t i = 0; i < func->instruction_count; i++) {
    const IRInstruction *in = &func->instructions[i];
    if (!ir_instruction_writes_destination(in) || !in->dest.name ||
        (in->dest.kind != IR_OPERAND_TEMP &&
         in->dest.kind != IR_OPERAND_SYMBOL)) {
      continue;
    }
    ptx_count_one_definition(fn, in->dest.name);
  }
}

static int ptx_rounded_arithmetic(const IRProgram *program,
                                  const IRFunction *function) {
  if (function->numerics_contract || function->is_host_twin) {
    return 1;
  }
  if (function->is_kernel) {
    return 0;
  }
  for (size_t i = 0; i < program->function_count; i++) {
    if (program->functions[i] && program->functions[i]->is_kernel &&
        program->functions[i]->numerics_contract) {
      return 1;
    }
  }
  return 0;
}

static void emit_function(IRProgram *program, size_t fi, CodeGenerator *gen,
                          FILE *out, int target_arch, char target_variant,
                          int isa_major, int isa_minor,
                          int tensor_tuple_budget, char **error) {
  PtxEmit e = {0};
  (void)gen;
  e.program = program;
  e.func = program->functions[fi];
  e.symbol = ir_program_lookup_symbol(program, e.func->name);
  e.fn.program = program;
  e.fn.function = e.func;
  e.fn.function_symbol = e.symbol;
  e.fn.target_arch = target_arch;
  e.fn.target_variant = target_variant;
  e.fn.isa_major = isa_major;
  e.fn.isa_minor = isa_minor;
  e.fn.tensor_tuple_budget = tensor_tuple_budget;
  e.fn.rounded_arithmetic = ptx_rounded_arithmetic(program, e.func);
  ptx_count_definitions(&e.fn, e.func);
  ptx_collect_generic_names(&e.fn, e.func, e.symbol);
  ptx_emit_return_descriptor(&e);
  sanitize_into(e.func->name ? e.func->name : "kernel", e.ename,
                sizeof(e.ename));
  e.fn.reported = ir_explain_device_function(
      e.func->name, e.func->location.filename, e.func->location.line,
      e.func->is_kernel, e.func->kernel_block);
  ptx_mark_line(&e.fn, e.func->location.line);

  ptx_emit_signature(&e);
  ptx_emit_dynamic_workgroup(&e);
  ptx_emit_allocations(&e);
  ptx_tile_plan(&e.fn, e.func);
  ptx_emit_transfer_barrier(&e);
  ptx_emit_parameter_loads(&e);
  ptx_emit_allocation_pointers(&e);
  ptx_emit_locals(&e);
  if (e.fn.tile_count) {
    ptx_tile_lanes(&e.fn);
    ptx_tile_budget(&e.fn, e.func,
                    e.func->kernel_block[0] > 0
                        ? e.func->kernel_block[0] *
                              (e.func->kernel_block[1] > 0
                                   ? e.func->kernel_block[1]
                                   : 1) *
                              (e.func->kernel_block[2] > 0
                                   ? e.func->kernel_block[2]
                                   : 1)
                        : 0);
  }
  ptx_emit_body(&e, error, target_arch);

  if (e.fn.error) {
    if (error) {
      *error = e.fn.error;
    } else {
      free(e.fn.error);
    }
  } else {
    ptx_emit_write(&e, out);
  }
  ptx_emit_release(&e);
}

static int ptx_u32_immediate_beside_u32(const IROperand *imm,
                                        const PtxVal *other) {
  return imm->kind == IR_OPERAND_INT && imm->int_value >= 0 &&
         imm->int_value <= 0xFFFFFFFFLL && other->cls == PC_B32 &&
         other->is_unsigned;
}

static PtxClass ptx_integer_result_class(const IRInstruction *in,
                                         const PtxVal *la, const PtxVal *ra) {
  PtxClass lc = la->cls, rc = ra->cls;
  if (ptx_u32_immediate_beside_u32(&in->rhs, la)) rc = PC_B32;
  if (ptx_u32_immediate_beside_u32(&in->lhs, ra)) lc = PC_B32;
  return (lc == PC_B64 || rc == PC_B64) ? PC_B64 : PC_B32;
}

static void emit_bitcast(PtxFn *fn, const IRInstruction *in, const char *t) {
  int wide = !strcmp(t, "f64bits") ||
             (!strcmp(t, "bitsf") && in->float_bits != 32);
  PtxClass from = t[0] == 'f' ? (wide ? PC_F64 : PC_F32)
                              : (wide ? PC_B64 : PC_B32);
  PtxClass to = t[0] == 'f' ? (wide ? PC_B64 : PC_B32)
                            : (wide ? PC_F64 : PC_F32);
  char s[24];
  use_as(fn, &in->lhs, from, s);
  PtxVal dv = {0};
  dv.cls = to;
  dv = destination_value(fn, &in->dest, dv);
  char dn[24];
  reg_name(to, dv.idx, dn);
  sb_printf(&fn->body, "\tmov.b%d %s, %s;\n", wide ? 64 : 32, dn, s);
  if (in->dest.name) {
    bind_value(fn, in->dest.name, dv);
  }
}

static void emit_unary(PtxFn *fn, const IRInstruction *in) {
  const char *t = in->text ? in->text : "";
  if (!strcmp(t, "f32bits") || !strcmp(t, "f64bits") || !strcmp(t, "bitsf")) {
    emit_bitcast(fn, in, t);
    return;
  }
  PtxVal sv = operand_desc(fn, &in->lhs);
  PtxClass c = in->is_float ? (in->float_bits == 32 ? PC_F32 : PC_F64)
                            : (sv.cls == PC_B64 ? PC_B64 : PC_B32);
  char s[24];
  use_as(fn, &in->lhs, c, s);
  PtxVal dv = {0};
  dv.cls = c;
  dv = destination_value(fn, &in->dest, dv);
  char dn[24];
  reg_name(c, dv.idx, dn);
  if (!strcmp(t, "-")) {
    sb_printf(&fn->body, "\tneg.%s %s, %s;\n", type_suffix_for_class(c, 0),
              dn, s);
  } else if (!strcmp(t, "~")) {
    sb_printf(&fn->body, "\tnot.%s %s, %s;\n", c == PC_B64 ? "b64" : "b32",
              dn, s);
  } else if (!strcmp(t, "sqrt") && in->is_float) {
    sb_printf(&fn->body, "\tsqrt.rn.%s %s, %s;\n",
              c == PC_F32 ? "f32" : "f64", dn, s);
  } else if (!strcmp(t, "!")) {
    int p = new_reg(fn, PC_PRED);
    char pn[24];
    reg_name(PC_PRED, p, pn);
    sb_printf(&fn->body, "\tsetp.eq.%s %s, %s, 0;\n",
              c == PC_B64 ? "s64" : "s32", pn, s);
    sb_printf(&fn->body, "\tselp.u32 %s, 1, 0, %s;\n", dn, pn);
  } else {
    fn_error(fn, "PTX: unsupported unary op '%s'", t);
  }
  if (in->dest.name) {
    bind_value(fn, in->dest.name, dv);
  }
}

static void emit_binary(PtxFn *fn, const IRInstruction *in) {
  const char *t = in->text ? in->text : "+";
  PtxVal la = operand_desc(fn, &in->lhs);
  PtxVal ra = operand_desc(fn, &in->rhs);

  if (is_compare_op(t)) {
    PtxClass c = PC_B32;
    int is_float = 0, is_unsigned = 0;
    if (la.cls == PC_F32 || ra.cls == PC_F32 || in->is_float) {
      c = PC_F32;
      is_float = 1;
    }
    if (la.cls == PC_F64 || ra.cls == PC_F64) {
      c = PC_F64;
      is_float = 1;
    }
    if (!is_float && (la.cls == PC_B64 || ra.cls == PC_B64)) {
      c = PC_B64;
    }
    if (!is_float) {
      is_unsigned = la.is_unsigned || ra.is_unsigned;
    }
    char a[24], b[24];
    use_as(fn, &in->lhs, c, a);
    use_as(fn, &in->rhs, c, b);
    int p = new_reg(fn, PC_PRED);
    char pn[24];
    reg_name(PC_PRED, p, pn);
    sb_printf(&fn->body, "\tsetp.%s.%s %s, %s, %s;\n",
              setp_cmp(t, is_float, is_unsigned),
              type_suffix_for_class(c, is_unsigned), pn, a, b);
    PtxVal dv = {.cls = PC_B32, .is_unsigned = 1};
    dv = destination_value(fn, &in->dest, dv);
    char dn[24];
    reg_name(PC_B32, dv.idx, dn);
    sb_printf(&fn->body, "\tselp.u32 %s, 1, 0, %s;\n", dn, pn);
    if (in->dest.name) {
      bind_value(fn, in->dest.name, dv);
    }
    return;
  }

  int is_logical = (!strcmp(t, "&&") || !strcmp(t, "||"));

  PtxVal dv = {0};
  if (in->is_float) {
    dv.cls = (in->float_bits == 32) ? PC_F32 : PC_F64;
  } else if (la.is_ptr || ra.is_ptr) {
    dv.cls = PC_B64;
    dv.is_ptr = 1;
    dv.is_unsigned = 1;
    dv.elem = la.is_ptr ? la.elem : ra.elem;
    dv.address_space =
        la.is_ptr ? la.address_space : ra.address_space;
    dv.ptr_align = ptx_offset_alignment(in, &la, &ra);
  } else {
    dv.cls = ptx_integer_result_class(in, &la, &ra);
    dv.is_unsigned = la.is_unsigned || ra.is_unsigned;
  }

  char a[24], b[24];
  use_as(fn, &in->lhs, dv.cls, a);
  use_as(fn, &in->rhs, dv.cls, b);
  int computed_ptr = dv.is_ptr;
  MtlcAddressSpace computed_space = dv.address_space;
  dv = destination_value(fn, &in->dest, dv);
  char dn[24];
  reg_name(dv.cls, dv.idx, dn);

  const char *ts = type_suffix_for_class(dv.cls, dv.is_unsigned);
  const char *bts = (dv.cls == PC_B64) ? "b64" : "b32";

  // An integer divisor known here goes in as an immediate rather than
  // through a mov.
  char divisor[32];
  const char *db = b;
  int is_integer = dv.cls != PC_F32 && dv.cls != PC_F64;
  if (is_integer && in->rhs.kind == IR_OPERAND_INT &&
      in->rhs.int_value > 0 &&
      (dv.cls == PC_B64 || in->rhs.int_value <= 0x7FFFFFFFLL)) {
    snprintf(divisor, sizeof(divisor), "%lld", in->rhs.int_value);
    db = divisor;
  }

  // Remainder by a power of two, and unsigned division by one, become
  // masks and shifts -- the remainder with the toward-zero correction for
  // signed operands, which adds 2^k - 1 to a negative dividend first.
  // ptxas keeps its general reciprocal routine for div/rem even when the
  // divisor is an immediate 4. Signed division stays div: as straight-line
  // shifts it lets ptxas hoist every loop-invariant fragment address of a
  // runtime-K tensor loop (the GB10 MXFP4/NVFP4 fixtures went from 42 and
  // 56 registers to 255); the IR rewrites already turn a provably
  // non-negative x / 2^k into a shift.
  int bits = dv.cls == PC_B64 ? 64 : 32;
  int log2 = -1;
  if (is_integer && db != b &&
      (!strcmp(t, "%") || (!strcmp(t, "/") && dv.is_unsigned))) {
    unsigned long long value = (unsigned long long)in->rhs.int_value;
    if ((value & (value - 1)) == 0) {
      log2 = 0;
      while ((1ULL << log2) != value) log2++;
      if (log2 >= bits - 1) log2 = -1;
    }
  }
  if (log2 >= 0) {
    int is_div = !strcmp(t, "/");
    unsigned long long low = (1ULL << log2) - 1;
    unsigned long long high = bits == 64 ? ~low : (~low & 0xFFFFFFFFULL);
    const char *sts = bits == 64 ? "s64" : "s32";
    const char *uts = bits == 64 ? "u64" : "u32";
    if (dv.is_unsigned) {
      if (is_div)
        sb_printf(&fn->body, "\tshr.%s %s, %s, %d;\n", uts, dn, a, log2);
      else
        sb_printf(&fn->body, "\tand.%s %s, %s, 0x%llX;\n", bts, dn, a, low);
    } else if (log2 == 0) {
      if (is_div)
        sb_printf(&fn->body, "\tmov.%s %s, %s;\n", bts, dn, a);
      else
        sb_printf(&fn->body, "\tmov.%s %s, 0;\n", bts, dn);
    } else {
      char biased[24];
      reg_name(dv.cls, new_reg(fn, dv.cls), biased);
      sb_printf(&fn->body, "\tshr.%s %s, %s, %d;\n", sts, biased, a,
                bits - 1);
      sb_printf(&fn->body, "\tshr.%s %s, %s, %d;\n", uts, biased, biased,
                bits - log2);
      sb_printf(&fn->body, "\tadd.%s %s, %s, %s;\n", sts, biased, a, biased);
      if (is_div) {
        sb_printf(&fn->body, "\tshr.%s %s, %s, %d;\n", sts, dn, biased,
                  log2);
      } else {
        sb_printf(&fn->body, "\tand.%s %s, %s, 0x%llX;\n", bts, biased,
                  biased, high);
        sb_printf(&fn->body, "\tsub.%s %s, %s, %s;\n", sts, dn, a, biased);
      }
    }
    if (in->dest.name) bind_value(fn, in->dest.name, dv);
    return;
  }

  const char *rounding =
      fn->rounded_arithmetic && (dv.cls == PC_F32 || dv.cls == PC_F64) ? "rn."
                                                                     : "";
  if (!strcmp(t, "+")) {
    sb_printf(&fn->body, "\tadd.%s%s %s, %s, %s;\n", rounding, ts, dn, a, b);
  } else if (!strcmp(t, "-")) {
    sb_printf(&fn->body, "\tsub.%s%s %s, %s, %s;\n", rounding, ts, dn, a, b);
  } else if (!strcmp(t, "*")) {
    if (dv.cls == PC_F32 || dv.cls == PC_F64) {
      sb_printf(&fn->body, "\tmul.%s%s %s, %s, %s;\n", rounding, ts, dn, a,
                b);
    } else {
      sb_printf(&fn->body, "\tmul.lo.%s %s, %s, %s;\n", ts, dn, a, b);
    }
  } else if (!strcmp(t, "/")) {
    if (dv.cls == PC_F32) {
      sb_printf(&fn->body, "\tdiv.rn.f32 %s, %s, %s;\n", dn, a, b);
    } else if (dv.cls == PC_F64) {
      sb_printf(&fn->body, "\tdiv.rn.f64 %s, %s, %s;\n", dn, a, b);
    } else {
      sb_printf(&fn->body, "\tdiv.%s %s, %s, %s;\n", ts, dn, a, db);
    }
  } else if (!strcmp(t, "%")) {
    sb_printf(&fn->body, "\trem.%s %s, %s, %s;\n", ts, dn, a, db);
  } else if (!strcmp(t, "&") || (is_logical && !strcmp(t, "&&"))) {
    sb_printf(&fn->body, "\tand.%s %s, %s, %s;\n", bts, dn, a, b);
  } else if (!strcmp(t, "|") || (is_logical && !strcmp(t, "||"))) {
    sb_printf(&fn->body, "\tor.%s %s, %s, %s;\n", bts, dn, a, b);
  } else if (!strcmp(t, "^")) {
    sb_printf(&fn->body, "\txor.%s %s, %s, %s;\n", bts, dn, a, b);
  } else if (!strcmp(t, "<<")) {
    char sh[24];
    use_as(fn, &in->rhs, PC_B32, sh);
    sb_printf(&fn->body, "\tshl.%s %s, %s, %s;\n", bts, dn, a, sh);
  } else if (!strcmp(t, ">>")) {
    char sh[24];
    use_as(fn, &in->rhs, PC_B32, sh);
    sb_printf(&fn->body, "\tshr.%s %s, %s, %s;\n",
              type_suffix_for_class(dv.cls, la.is_unsigned), dn, a, sh);
  } else {
    fn_error(fn, "PTX: unsupported binary op '%s'", t);
  }
  if (computed_ptr && dv.is_ptr) {
    ptx_convert_space_into(fn, computed_space, dv.address_space, dn, dn);
  }
  if (in->dest.name) {
    bind_value(fn, in->dest.name, dv);
  }
}
