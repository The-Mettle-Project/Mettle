#include "msl_emitter.h"
#include "gpu_structure.h"
#include "ir/ir_device_report.h"
#include "ir/ir_explain_device.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  char *data;
  size_t len;
  size_t cap;
} Sb;

static void sb_reserve(Sb *sb, size_t extra) {
  size_t need = sb->len + extra + 1;
  char *grown;
  if (need <= sb->cap) {
    return;
  }
  size_t next = sb->cap ? sb->cap * 2 : 256;
  while (next < need) {
    next *= 2;
  }
  grown = realloc(sb->data, next);
  if (!grown) {
    return;
  }
  sb->data = grown;
  sb->cap = next;
}

static void sb_puts(Sb *sb, const char *s) {
  size_t n = strlen(s);
  sb_reserve(sb, n);
  if (sb->len + n + 1 > sb->cap) {
    return;
  }
  memcpy(sb->data + sb->len, s, n);
  sb->len += n;
  sb->data[sb->len] = '\0';
}

static void sb_printf(Sb *sb, const char *format, ...) {
  va_list args;
  int needed;
  va_start(args, format);
  needed = vsnprintf(NULL, 0, format, args);
  va_end(args);
  if (needed < 0) {
    return;
  }
  sb_reserve(sb, (size_t)needed);
  if (sb->len + (size_t)needed + 1 > sb->cap) {
    return;
  }
  va_start(args, format);
  vsnprintf(sb->data + sb->len, (size_t)needed + 1, format, args);
  va_end(args);
  sb->len += (size_t)needed;
}

static void sb_free(Sb *sb) {
  free(sb->data);
  sb->data = NULL;
  sb->len = sb->cap = 0;
}

static const char *sb_str(const Sb *sb) { return sb->data ? sb->data : ""; }

typedef enum {
  MC_NONE = 0,
  MC_I32,
  MC_U32,
  MC_I64,
  MC_U64,
  MC_F32,
  MC_F64,
  MC_PTR,
  MC_REC
} MslClass;

typedef enum {
  MS_NONE = 0,
  MS_DEVICE,
  MS_TG,
  MS_THREAD,
  MS_CONFLICT
} MslSpace;

enum {
  MB_TID = 1u,
  MB_CTAID = 2u,
  MB_NTID = 4u,
  MB_NCTAID = 8u,
  MB_LANE = 16u,
  MB_LANES = 32u
};

enum {
  NEED_DP4A_U = 1u,
  NEED_DP4A_S = 2u,
  NEED_DP2A_U = 4u,
  NEED_DP2A_S = 8u,
  NEED_PRMT = 16u
};

typedef struct {
  char *name;
  char ident[192];
  MslClass cls;
  MtlcTypeKind storage;
  int fixed;
  MslSpace space;
  MslSpace conflict_with;
  MtlcTypeKind elem;
  const MtlcType *rec;
  int is_param;
  int alloc;
  long long alloc_count;
  MtlcTypeKind alloc_elem;
  const MtlcType *alloc_elem_type;
  MslSpace alloc_space;
  int demoted;
  const char *literal;
} MslVar;

struct MslMod;
struct MslSpec;

typedef struct {
  size_t lo;
  size_t hi;
  const IRInstruction *term;
  const char *label;
} MslBlock;

typedef struct MslFn {
  struct MslMod *m;
  IRFunction *func;
  size_t function_index;
  const IRModuleSymbol *symbol;
  struct MslSpec *spec;
  MslVar *vars;
  size_t nvars;
  size_t capvars;
  int returns_void;
  MslClass ret_cls;
  MtlcTypeKind ret_kind;
  const MtlcType *ret_rec;
  MslSpace ret_space;
  unsigned builtins;
  int has_dynamic;
  MslBlock *blocks;
  size_t nblocks;
  Sb out;
  int indent;
  size_t temp_counter;
  uint32_t *resident;
  size_t resident_count;
  int reported;
} MslFn;

typedef struct MslSpec {
  size_t function_index;
  MslSpace *param_space;
  size_t param_count;
  char name[256];
  unsigned builtins;
  MslSpace ret_space;
  size_t *call_spec;
  size_t instruction_count;
  MslFn *fn;
  int coherent;
} MslSpec;

typedef struct {
  const MtlcType *type;
  char name[160];
} MslRecord;

typedef struct MslMod {
  IRProgram *program;
  MslEmitOptions options;
  IRGpuCallGraph graph;
  MslSpec *specs;
  size_t nspecs;
  size_t capspecs;
  MslRecord *records;
  size_t nrecords;
  size_t caprecords;
  Sb records_out;
  unsigned needs;
  int coherent_device;
  const IRInstruction *current;
  char *error;
} MslMod;

static void mod_error(MslMod *m, const char *format, ...) {
  char buffer[1024];
  va_list args;
  if (m->error) {
    return;
  }
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  if (m->current && m->current->location.line) {
    size_t used = strlen(buffer);
    char dump[256];
    dump[0] = 0;
    if (getenv("METTLE_METAL_DEBUG")) {
      ir_instruction_dump(m->current, dump, sizeof(dump));
    }
    snprintf(buffer + used, sizeof(buffer) - used, " (line %zu%s%s)",
             m->current->location.line, dump[0] ? ": " : "", dump);
  }
  m->error = strdup(buffer);
}

static int version_at_least(const MslMod *m, int major, int minor) {
  return m->options.version_major > major ||
         (m->options.version_major == major &&
          m->options.version_minor >= minor);
}

static const char *source_name(const char *name) {
  if (!name) {
    return "?";
  }
  while (*name == '.' || *name == '%' || *name == '@') {
    name++;
  }
  return name;
}

static const char *fn_name(const MslFn *fn) {
  return fn->func && fn->func->name ? fn->func->name : "?";
}

static MslClass class_of_kind(MtlcTypeKind kind) {
  switch (kind) {
  case MTLC_TYPE_INT8:
  case MTLC_TYPE_INT16:
  case MTLC_TYPE_INT32:
  case MTLC_TYPE_ENUM:
    return MC_I32;
  case MTLC_TYPE_UINT8:
  case MTLC_TYPE_UINT16:
  case MTLC_TYPE_UINT32:
  case MTLC_TYPE_BOOL:
    return MC_U32;
  case MTLC_TYPE_INT64:
    return MC_I64;
  case MTLC_TYPE_UINT64:
    return MC_U64;
  case MTLC_TYPE_FLOAT32:
  case MTLC_TYPE_FLOAT16:
  case MTLC_TYPE_BFLOAT16:
    return MC_F32;
  case MTLC_TYPE_FLOAT64:
    return MC_F64;
  case MTLC_TYPE_POINTER:
  case MTLC_TYPE_STRING:
  case MTLC_TYPE_FUNCTION_POINTER:
    return MC_PTR;
  case MTLC_TYPE_STRUCT:
  case MTLC_TYPE_ARRAY:
  case MTLC_TYPE_TAGGED_ENUM:
    return MC_REC;
  default:
    return MC_NONE;
  }
}

static MtlcTypeKind natural_kind(MslClass cls) {
  switch (cls) {
  case MC_I32: return MTLC_TYPE_INT32;
  case MC_U32: return MTLC_TYPE_UINT32;
  case MC_I64: return MTLC_TYPE_INT64;
  case MC_U64: return MTLC_TYPE_UINT64;
  case MC_F32: return MTLC_TYPE_FLOAT32;
  case MC_F64: return MTLC_TYPE_FLOAT64;
  case MC_PTR: return MTLC_TYPE_POINTER;
  default: return MTLC_TYPE_VOID;
  }
}

static const char *class_type(MslClass cls) {
  switch (cls) {
  case MC_I32: return "int";
  case MC_U32: return "uint";
  case MC_I64: return "long";
  case MC_U64: return "ulong";
  case MC_F32: return "float";
  case MC_F64: return "double";
  default: return "uint";
  }
}

static const char *unsigned_type(MslClass cls) {
  return (cls == MC_I64 || cls == MC_U64) ? "ulong" : "uint";
}

static const char *signed_type(MslClass cls) {
  return (cls == MC_I64 || cls == MC_U64) ? "long" : "int";
}

static int class_is_int(MslClass cls) {
  return cls == MC_I32 || cls == MC_U32 || cls == MC_I64 || cls == MC_U64;
}

static int class_is_64(MslClass cls) {
  return cls == MC_I64 || cls == MC_U64 || cls == MC_PTR;
}

static int class_is_unsigned(MslClass cls) {
  return cls == MC_U32 || cls == MC_U64;
}

static const char *storage_type(MtlcTypeKind kind) {
  switch (kind) {
  case MTLC_TYPE_INT8: return "char";
  case MTLC_TYPE_UINT8: return "uchar";
  case MTLC_TYPE_BOOL: return "uchar";
  case MTLC_TYPE_INT16: return "short";
  case MTLC_TYPE_UINT16: return "ushort";
  case MTLC_TYPE_INT32: return "int";
  case MTLC_TYPE_ENUM: return "int";
  case MTLC_TYPE_UINT32: return "uint";
  case MTLC_TYPE_INT64: return "long";
  case MTLC_TYPE_UINT64: return "ulong";
  case MTLC_TYPE_FLOAT32: return "float";
  case MTLC_TYPE_FLOAT16: return "half";
  case MTLC_TYPE_BFLOAT16: return "bfloat";
  case MTLC_TYPE_FLOAT64: return "double";
  default: return NULL;
  }
}

static size_t kind_size(MtlcTypeKind kind) {
  switch (kind) {
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
  case MTLC_TYPE_ENUM:
    return 4;
  default:
    return 8;
  }
}

static const char *space_word(MslSpace space) {
  switch (space) {
  case MS_TG: return "threadgroup";
  case MS_THREAD: return "thread";
  default: return "device";
  }
}

static const char *space_name(MslSpace space) {
  switch (space) {
  case MS_TG: return "a threadgroup";
  case MS_THREAD: return "a thread-private";
  case MS_DEVICE: return "a device";
  default: return "an unknown";
  }
}

static MslSpace space_of_mtlc(MtlcAddressSpace space) {
  switch (space) {
  case MTLC_ADDRESS_SPACE_WORKGROUP: return MS_TG;
  case MTLC_ADDRESS_SPACE_PRIVATE: return MS_THREAD;
  case MTLC_ADDRESS_SPACE_GLOBAL:
  case MTLC_ADDRESS_SPACE_CONSTANT:
    return MS_DEVICE;
  default:
    return MS_NONE;
  }
}

static const char *device_qual(const MslMod *m) {
  return m->coherent_device ? "coherent(device) device" : "device";
}

static const char *space_qual(const MslMod *m, MslSpace space) {
  if (space == MS_DEVICE || space == MS_NONE || space == MS_CONFLICT) {
    return device_qual(m);
  }
  return space_word(space);
}

static MtlcTypeKind kind_from_name(const char *s) {
  int is_unsigned;
  if (!s) {
    return MTLC_TYPE_INT32;
  }
  if (strchr(s, '*') || strstr(s, "cstring") || !strcmp(s, "string")) {
    return MTLC_TYPE_POINTER;
  }
  is_unsigned = strstr(s, "uint") != NULL;
  if (strstr(s, "bfloat16")) return MTLC_TYPE_BFLOAT16;
  if (strstr(s, "float16")) return MTLC_TYPE_FLOAT16;
  if (strstr(s, "float32")) return MTLC_TYPE_FLOAT32;
  if (strstr(s, "float64") || strstr(s, "float")) return MTLC_TYPE_FLOAT64;
  if (strstr(s, "int64")) return is_unsigned ? MTLC_TYPE_UINT64 : MTLC_TYPE_INT64;
  if (strstr(s, "int16")) return is_unsigned ? MTLC_TYPE_UINT16 : MTLC_TYPE_INT16;
  if (strstr(s, "int8")) return is_unsigned ? MTLC_TYPE_UINT8 : MTLC_TYPE_INT8;
  if (strstr(s, "int32")) return is_unsigned ? MTLC_TYPE_UINT32 : MTLC_TYPE_INT32;
  if (strstr(s, "bool")) return MTLC_TYPE_BOOL;
  if (!strcmp(s, "void")) return MTLC_TYPE_VOID;
  return MTLC_TYPE_INT32;
}

static MtlcTypeKind type_kind(const MtlcType *type, const char *fallback) {
  if (type) {
    if (type->kind == MTLC_TYPE_ENUM) {
      return type->size == 8   ? MTLC_TYPE_INT64
             : type->size == 2 ? MTLC_TYPE_INT16
             : type->size == 1 ? MTLC_TYPE_INT8
                               : MTLC_TYPE_INT32;
    }
    return type->kind;
  }
  return kind_from_name(fallback);
}

static MtlcTypeKind pointee_kind(const MtlcType *type, const char *fallback) {
  if (type && type->kind == MTLC_TYPE_POINTER) {
    if (!type->base_type) {
      return MTLC_TYPE_VOID;
    }
    if (type->base_type->kind == MTLC_TYPE_STRUCT ||
        type->base_type->kind == MTLC_TYPE_ARRAY ||
        type->base_type->kind == MTLC_TYPE_TAGGED_ENUM) {
      return MTLC_TYPE_VOID;
    }
    return type_kind(type->base_type, NULL);
  }
  if (fallback) {
    const char *star = strchr(fallback, '*');
    char base[128];
    size_t n;
    if (!star) {
      return MTLC_TYPE_VOID;
    }
    if (strchr(star + 1, '*')) {
      return MTLC_TYPE_POINTER;
    }
    n = (size_t)(star - fallback);
    if (n >= sizeof(base)) {
      n = sizeof(base) - 1;
    }
    memcpy(base, fallback, n);
    base[n] = '\0';
    return kind_from_name(base);
  }
  return MTLC_TYPE_VOID;
}

static int type_is_record(const MtlcType *type) {
  return type && (type->kind == MTLC_TYPE_STRUCT ||
                  type->kind == MTLC_TYPE_ARRAY ||
                  type->kind == MTLC_TYPE_TAGGED_ENUM);
}

static MslVar *find_var(MslFn *fn, const char *name) {
  if (!name) {
    return NULL;
  }
  for (size_t i = 0; i < fn->nvars; i++) {
    if (strcmp(fn->vars[i].name, name) == 0) {
      return &fn->vars[i];
    }
  }
  return NULL;
}

static void make_ident(MslFn *fn, MslVar *var, int is_symbol) {
  const char *name = source_name(var->name);
  char base[160];
  size_t n = 0;
  int suffix = 0;
  for (const char *p = name; *p && n + 1 < sizeof(base); p++) {
    char c = *p;
    int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
             (c >= '0' && c <= '9');
    base[n++] = ok ? c : '_';
  }
  base[n] = '\0';
  for (;;) {
    int clash = 0;
    if (suffix == 0) {
      snprintf(var->ident, sizeof(var->ident), "%s%s",
               is_symbol ? "m_" : "r_", base);
    } else {
      snprintf(var->ident, sizeof(var->ident), "%s%s_%d",
               is_symbol ? "m_" : "r_", base, suffix);
    }
    for (size_t i = 0; i < fn->nvars; i++) {
      if (&fn->vars[i] != var && strcmp(fn->vars[i].ident, var->ident) == 0) {
        clash = 1;
        break;
      }
    }
    if (!clash) {
      return;
    }
    suffix++;
  }
}

static MslVar *add_var(MslFn *fn, const char *name, int is_symbol) {
  MslVar *var = find_var(fn, name);
  if (var || !name) {
    return var;
  }
  if (fn->nvars == fn->capvars) {
    size_t next = fn->capvars ? fn->capvars * 2 : 32;
    MslVar *grown = realloc(fn->vars, next * sizeof(MslVar));
    if (!grown) {
      mod_error(fn->m, "Metal: out of memory");
      return NULL;
    }
    fn->vars = grown;
    fn->capvars = next;
  }
  var = &fn->vars[fn->nvars++];
  memset(var, 0, sizeof(*var));
  var->name = strdup(name);
  var->storage = MTLC_TYPE_VOID;
  var->elem = MTLC_TYPE_VOID;
  make_ident(fn, var, is_symbol);
  return var;
}

static void join_space(MslVar *var, MslSpace space, int *changed) {
  if (space == MS_NONE || var->space == space ||
      var->space == MS_CONFLICT) {
    return;
  }
  if (var->space == MS_NONE) {
    var->space = space;
  } else {
    var->conflict_with = space;
    var->space = MS_CONFLICT;
  }
  *changed = 1;
}

static void declare_var(MslFn *fn, const char *name, const MtlcType *type,
                        const char *type_name, int is_symbol) {
  MslVar *var = add_var(fn, name, is_symbol);
  MtlcTypeKind kind;
  if (!var || var->fixed) {
    return;
  }
  kind = type_kind(type, type_name);
  var->fixed = 1;
  if (type_is_record(type)) {
    var->cls = MC_REC;
    var->rec = type;
    return;
  }
  var->cls = class_of_kind(kind);
  if (var->cls == MC_NONE) {
    var->cls = MC_I32;
  }
  if (var->cls == MC_PTR) {
    var->elem = pointee_kind(type, type_name);
    if (type && type->kind == MTLC_TYPE_POINTER) {
      MslSpace declared = space_of_mtlc(type->address_space);
      if (declared != MS_NONE) {
        int changed = 0;
        join_space(var, declared, &changed);
      }
    }
    return;
  }
  var->storage = kind;
}

static MslSpec *spec_of_index(MslMod *m, size_t index) {
  return index < m->nspecs ? &m->specs[index] : NULL;
}

static const MtlcType *param_type(const MslFn *fn, size_t p) {
  if (fn->symbol && fn->symbol->kind == IR_MODSYM_FUNCTION &&
      p < fn->symbol->param_count) {
    return fn->symbol->param_types[p];
  }
  return NULL;
}

static const char *param_type_name(const MslFn *fn, size_t p) {
  return fn->func->parameter_types ? fn->func->parameter_types[p] : NULL;
}

static IRFunction *lookup_function(MslMod *m, const char *name,
                                   size_t *index) {
  if (!name) {
    return NULL;
  }
  for (size_t i = 0; i < m->program->function_count; i++) {
    IRFunction *f = m->program->functions[i];
    if (f && f->name && strcmp(f->name, name) == 0) {
      if (index) {
        *index = i;
      }
      return f;
    }
  }
  return NULL;
}

static const MtlcType *return_type_of(MslMod *m, IRFunction *func) {
  const IRModuleSymbol *symbol = ir_program_lookup_symbol(m->program,
                                                          func->name);
  if (symbol && symbol->kind == IR_MODSYM_FUNCTION && symbol->return_type) {
    return symbol->return_type;
  }
  return func->return_type_name
             ? ir_program_lookup_type(m->program, func->return_type_name)
             : NULL;
}

static int returns_void_of(MslMod *m, IRFunction *func) {
  const MtlcType *type = return_type_of(m, func);
  if (type) {
    return type->kind == MTLC_TYPE_VOID;
  }
  return !func->return_type_name || !strcmp(func->return_type_name, "void");
}

static int builtin_component(MtlcIntrinsic intrinsic, unsigned *mask,
                             const char **base) {
  switch (intrinsic) {
  case MTLC_INTRINSIC_GPU_LOCAL_ID_X: *mask = MB_TID; *base = "mtl_tid"; return 0;
  case MTLC_INTRINSIC_GPU_LOCAL_ID_Y: *mask = MB_TID; *base = "mtl_tid"; return 1;
  case MTLC_INTRINSIC_GPU_LOCAL_ID_Z: *mask = MB_TID; *base = "mtl_tid"; return 2;
  case MTLC_INTRINSIC_GPU_LOCAL_SIZE_X: *mask = MB_NTID; *base = "mtl_ntid"; return 0;
  case MTLC_INTRINSIC_GPU_LOCAL_SIZE_Y: *mask = MB_NTID; *base = "mtl_ntid"; return 1;
  case MTLC_INTRINSIC_GPU_LOCAL_SIZE_Z: *mask = MB_NTID; *base = "mtl_ntid"; return 2;
  case MTLC_INTRINSIC_GPU_GROUP_ID_X: *mask = MB_CTAID; *base = "mtl_ctaid"; return 0;
  case MTLC_INTRINSIC_GPU_GROUP_ID_Y: *mask = MB_CTAID; *base = "mtl_ctaid"; return 1;
  case MTLC_INTRINSIC_GPU_GROUP_ID_Z: *mask = MB_CTAID; *base = "mtl_ctaid"; return 2;
  case MTLC_INTRINSIC_GPU_NUM_GROUPS_X: *mask = MB_NCTAID; *base = "mtl_nctaid"; return 0;
  case MTLC_INTRINSIC_GPU_NUM_GROUPS_Y: *mask = MB_NCTAID; *base = "mtl_nctaid"; return 1;
  case MTLC_INTRINSIC_GPU_NUM_GROUPS_Z: *mask = MB_NCTAID; *base = "mtl_nctaid"; return 2;
  default: return -1;
  }
}

static int is_math_intrinsic(MtlcIntrinsic intrinsic) {
  return intrinsic >= MTLC_INTRINSIC_GPU_SQRT_F32 &&
         intrinsic <= MTLC_INTRINSIC_GPU_EXP_F32;
}

static MslClass intrinsic_class(MtlcIntrinsic intrinsic) {
  const char *base;
  unsigned mask;
  if (builtin_component(intrinsic, &mask, &base) >= 0) {
    return MC_U32;
  }
  switch (intrinsic) {
  case MTLC_INTRINSIC_GPU_SUBGROUP_LOCAL_ID:
  case MTLC_INTRINSIC_GPU_SUBGROUP_SIZE:
    return MC_U32;
  case MTLC_INTRINSIC_GPU_F16_BITS_TO_F32:
  case MTLC_INTRINSIC_GPU_F32_FROM_BITS:
  case MTLC_INTRINSIC_GPU_H2F_LO:
  case MTLC_INTRINSIC_GPU_H2F_HI:
  case MTLC_INTRINSIC_GPU_BF2F:
    return MC_F32;
  case MTLC_INTRINSIC_GPU_DP4A_S32:
  case MTLC_INTRINSIC_GPU_DP2A_LO_S32:
  case MTLC_INTRINSIC_GPU_DP2A_HI_S32:
    return MC_I32;
  case MTLC_INTRINSIC_GPU_F32_TO_F16_BITS:
  case MTLC_INTRINSIC_GPU_F32_TO_BITS:
  case MTLC_INTRINSIC_GPU_DP4A_U32:
  case MTLC_INTRINSIC_GPU_DP2A_LO_U32:
  case MTLC_INTRINSIC_GPU_DP2A_HI_U32:
  case MTLC_INTRINSIC_GPU_PRMT_B32:
  case MTLC_INTRINSIC_GPU_HADD2:
  case MTLC_INTRINSIC_GPU_HMUL2:
  case MTLC_INTRINSIC_GPU_HFMA2:
  case MTLC_INTRINSIC_GPU_F2H2:
  case MTLC_INTRINSIC_GPU_F2BF:
  case MTLC_INTRINSIC_GPU_WORKGROUP_UNIFORM_U32:
    return MC_U32;
  default:
    break;
  }
  if (is_math_intrinsic(intrinsic)) {
    return MC_F32;
  }
  if (ir_intrinsic_is_atomic(intrinsic)) {
    return ir_intrinsic_atomic_result_kind(intrinsic) == MTLC_TYPE_UINT64
               ? MC_U64
               : MC_U32;
  }
  if (ir_intrinsic_is_subgroup(intrinsic)) {
    MtlcTypeKind kind = ir_intrinsic_subgroup_result_kind(intrinsic);
    return kind == MTLC_TYPE_FLOAT32 ? MC_F32
           : kind == MTLC_TYPE_BOOL  ? MC_U32
                                     : class_of_kind(kind);
  }
  return MC_NONE;
}

static MslClass literal_class(const IROperand *op) {
  if (op->kind == IR_OPERAND_FLOAT) {
    return MC_F32;
  }
  if (op->int_value > 2147483647LL || op->int_value < -2147483648LL) {
    return MC_I64;
  }
  return MC_I32;
}

static MslClass operand_class(MslFn *fn, const IROperand *op) {
  MslVar *var;
  if (op->kind == IR_OPERAND_INT || op->kind == IR_OPERAND_FLOAT) {
    return literal_class(op);
  }
  var = find_var(fn, op->name);
  return var && var->cls != MC_NONE ? var->cls : MC_I32;
}

static MslVar *operand_var(MslFn *fn, const IROperand *op) {
  if (op->kind != IR_OPERAND_TEMP && op->kind != IR_OPERAND_SYMBOL) {
    return NULL;
  }
  return find_var(fn, op->name);
}

static MslSpace operand_space(MslFn *fn, const IROperand *op) {
  MslVar *var = operand_var(fn, op);
  return var && var->cls == MC_PTR ? var->space : MS_NONE;
}

static int is_compare(const char *t) {
  return !strcmp(t, "<") || !strcmp(t, ">") || !strcmp(t, "<=") ||
         !strcmp(t, ">=") || !strcmp(t, "==") || !strcmp(t, "!=") ||
         !strcmp(t, "&&") || !strcmp(t, "||");
}

static MtlcTypeKind access_kind(MslFn *fn, const IROperand *address,
                                const IRInstruction *in) {
  MslVar *var = operand_var(fn, address);
  long long size = in->rhs.kind == IR_OPERAND_INT ? in->rhs.int_value : 4;
  if (var && var->cls == MC_PTR && var->elem != MTLC_TYPE_VOID &&
      (long long)kind_size(var->elem) == size) {
    if (in->is_float == (var->elem == MTLC_TYPE_FLOAT32 ||
                         var->elem == MTLC_TYPE_FLOAT64 ||
                         var->elem == MTLC_TYPE_FLOAT16 ||
                         var->elem == MTLC_TYPE_BFLOAT16)) {
      return var->elem;
    }
  }
  if (in->is_float) {
    if (size == 2) {
      return in->alias_class == IR_ALIAS_CLASS_BF16 ? MTLC_TYPE_BFLOAT16
                                                    : MTLC_TYPE_FLOAT16;
    }
    return size == 4 ? MTLC_TYPE_FLOAT32 : MTLC_TYPE_FLOAT64;
  }
  return size == 8   ? MTLC_TYPE_INT64
         : size == 2 ? MTLC_TYPE_INT16
         : size == 1 ? MTLC_TYPE_UINT8
                     : MTLC_TYPE_INT32;
}

static MtlcTypeKind cast_kind(const IRInstruction *in) {
  return type_kind(in->value_type, in->text);
}

typedef struct {
  MslClass cls;
  MslSpace space;
  MtlcTypeKind elem;
  const MtlcType *rec;
} MslFacts;

static void infer_binary(MslFn *fn, const IRInstruction *in, MslFacts *r) {
  const char *t = in->text ? in->text : "+";
  MslClass a = operand_class(fn, &in->lhs);
  MslClass b = operand_class(fn, &in->rhs);
  if (is_compare(t)) {
    r->cls = MC_U32;
  } else if (in->is_float) {
    r->cls = in->float_bits == 64 ? MC_F64 : MC_F32;
  } else if (a == MC_PTR && b == MC_PTR) {
    r->cls = MC_I64;
  } else if (a == MC_PTR || b == MC_PTR) {
    MslVar *pointer = operand_var(fn, a == MC_PTR ? &in->lhs : &in->rhs);
    r->cls = MC_PTR;
    if (pointer) {
      r->space = pointer->space;
      r->elem = pointer->elem;
    }
  } else if (a == MC_F32 || b == MC_F32 || a == MC_F64 || b == MC_F64) {
    r->cls = (a == MC_F64 || b == MC_F64) ? MC_F64 : MC_F32;
  } else {
    int wide = class_is_64(a) || class_is_64(b);
    int uns = in->is_unsigned || class_is_unsigned(a) || class_is_unsigned(b);
    r->cls = wide ? (uns ? MC_U64 : MC_I64) : (uns ? MC_U32 : MC_I32);
  }
}

static void infer_unary(MslFn *fn, const IRInstruction *in, MslFacts *r) {
  MslClass a = operand_class(fn, &in->lhs);
  const char *t = in->text ? in->text : "";
  if (!strcmp(t, "!")) {
    r->cls = MC_U32;
  } else if (in->is_float) {
    r->cls = in->float_bits == 64 ? MC_F64 : MC_F32;
  } else {
    r->cls = class_is_int(a) ? a : MC_I32;
  }
}

static void infer_load(MslFn *fn, const IRInstruction *in, MslFacts *r) {
  MtlcTypeKind kind = access_kind(fn, &in->lhs, in);
  MslVar *pointer = operand_var(fn, &in->lhs);
  if (pointer && pointer->cls == MC_PTR && pointer->elem == MTLC_TYPE_POINTER &&
      in->rhs.kind == IR_OPERAND_INT && in->rhs.int_value == 8) {
    r->cls = MC_PTR;
    r->space = MS_DEVICE;
  } else {
    r->cls = class_of_kind(kind);
  }
}

static void infer_assign(MslFn *fn, const IRInstruction *in, MslFacts *r) {
  MslVar *source = operand_var(fn, &in->lhs);
  if (source) {
    r->cls = source->cls;
    r->space = source->space;
    r->elem = source->elem;
    r->rec = source->rec;
  } else {
    r->cls = literal_class(&in->lhs);
  }
}

static void infer_cast(MslFn *fn, const IRInstruction *in, MslFacts *r) {
  MtlcTypeKind kind = cast_kind(in);
  r->cls = class_of_kind(kind);
  if (r->cls == MC_PTR) {
    MslVar *source = operand_var(fn, &in->lhs);
    MslSpace declared = MS_NONE;
    r->elem = pointee_kind(in->value_type, in->text);
    if (in->value_type && in->value_type->kind == MTLC_TYPE_POINTER) {
      declared = space_of_mtlc(in->value_type->address_space);
    }
    r->space = declared != MS_NONE ? declared
               : (source && source->cls == MC_PTR) ? source->space
                                                   : MS_DEVICE;
    if (r->elem == MTLC_TYPE_VOID && source && source->cls == MC_PTR) {
      r->elem = source->elem;
    }
  } else if (r->cls == MC_REC) {
    r->rec = in->value_type;
  } else if (r->cls == MC_NONE) {
    r->cls = MC_I32;
  }
}

static void infer_select(MslFn *fn, const IRInstruction *in, MslFacts *r) {
  MslVar *source = operand_var(fn, &in->rhs);
  if (source) {
    r->cls = source->cls;
    r->space = source->space;
    r->elem = source->elem;
  } else {
    source = in->argument_count > 0 ? operand_var(fn, &in->arguments[0])
                                    : NULL;
    r->cls = source ? source->cls : literal_class(&in->rhs);
    if (source) {
      r->space = source->space;
      r->elem = source->elem;
    }
  }
  if (in->argument_count > 0) {
    MslSpace other = operand_space(fn, &in->arguments[0]);
    if (r->cls == MC_PTR && other != MS_NONE && r->space == MS_NONE) {
      r->space = other;
    }
  }
}

static void infer_address_of(MslFn *fn, const IRInstruction *in,
                             MslFacts *r) {
  MslVar *home = operand_var(fn, &in->lhs);
  r->cls = MC_PTR;
  r->space = MS_THREAD;
  if (home && home->cls != MC_REC && home->cls != MC_PTR) {
    r->elem = home->storage != MTLC_TYPE_VOID ? home->storage
                                              : natural_kind(home->cls);
  } else if (home && home->cls == MC_PTR) {
    r->elem = MTLC_TYPE_POINTER;
  }
}

static int infer_call(MslFn *fn, const IRInstruction *in, MslFacts *r) {
  if (in->intrinsic != MTLC_INTRINSIC_NONE) {
    r->cls = intrinsic_class(in->intrinsic);
    if (r->cls == MC_NONE) {
      return 0;
    }
  } else {
    size_t index = 0;
    IRFunction *callee = lookup_function(fn->m, in->text, &index);
    const MtlcType *type;
    if (!callee || returns_void_of(fn->m, callee)) {
      return 0;
    }
    type = return_type_of(fn->m, callee);
    if (type_is_record(type)) {
      r->cls = MC_REC;
      r->rec = type;
    } else {
      r->cls = class_of_kind(type_kind(type, callee->return_type_name));
      if (r->cls == MC_PTR) {
        r->space = MS_DEVICE;
        r->elem = pointee_kind(type, callee->return_type_name);
        if (fn->spec && fn->spec->call_spec &&
            (size_t)(in - fn->func->instructions) <
                fn->spec->instruction_count) {
          size_t target =
              fn->spec->call_spec[(size_t)(in - fn->func->instructions)];
          MslSpec *callee_spec = spec_of_index(fn->m, target);
          if (callee_spec && callee_spec->ret_space != MS_NONE) {
            r->space = callee_spec->ret_space;
          }
        }
      }
    }
  }
  return 1;
}

static void infer_instruction(MslFn *fn, const IRInstruction *in,
                              int *changed) {
  MslVar *dest;
  MslFacts r;
  r.cls = MC_NONE;
  r.space = MS_NONE;
  r.elem = MTLC_TYPE_VOID;
  r.rec = NULL;
  if (!in->dest.name ||
      (in->dest.kind != IR_OPERAND_TEMP && in->dest.kind != IR_OPERAND_SYMBOL)) {
    return;
  }
  switch (in->op) {
  case IR_OP_BINARY:
    infer_binary(fn, in, &r);
    break;
  case IR_OP_UNARY:
    infer_unary(fn, in, &r);
    break;
  case IR_OP_LOAD:
    infer_load(fn, in, &r);
    break;
  case IR_OP_ASSIGN:
    infer_assign(fn, in, &r);
    break;
  case IR_OP_CAST:
    infer_cast(fn, in, &r);
    break;
  case IR_OP_SELECT:
    infer_select(fn, in, &r);
    break;
  case IR_OP_ADDRESS_OF:
    infer_address_of(fn, in, &r);
    break;
  case IR_OP_CALL:
    if (!infer_call(fn, in, &r)) {
      return;
    }
    break;
  default:
    return;
  }
  dest = add_var(fn, in->dest.name, in->dest.kind == IR_OPERAND_SYMBOL);
  if (!dest) {
    return;
  }
  if (!dest->fixed) {
    if (dest->cls == MC_NONE && r.cls != MC_NONE) {
      dest->cls = r.cls;
      dest->rec = r.rec;
      *changed = 1;
    }
    if (dest->cls == MC_PTR && dest->elem == MTLC_TYPE_VOID &&
        r.elem != MTLC_TYPE_VOID) {
      dest->elem = r.elem;
      *changed = 1;
    }
  }
  if (dest->cls == MC_PTR && r.cls == MC_PTR) {
    join_space(dest, r.space, changed);
  }
}

static void register_signature(MslFn *fn) {
  IRFunction *func = fn->func;
  for (size_t p = 0; p < func->parameter_count; p++) {
    MslVar *var;
    if (!func->parameter_names || !func->parameter_names[p]) {
      continue;
    }
    declare_var(fn, func->parameter_names[p], param_type(fn, p),
                param_type_name(fn, p), 1);
    var = find_var(fn, func->parameter_names[p]);
    if (!var) {
      continue;
    }
    var->is_param = 1;
    if (var->cls == MC_PTR) {
      MslSpace given = fn->spec && p < fn->spec->param_count
                           ? fn->spec->param_space[p]
                           : MS_NONE;
      if (func->is_kernel && (given == MS_NONE)) {
        given = MS_DEVICE;
      }
      if (given != MS_NONE && var->space != MS_NONE && var->space != given) {
        mod_error(fn->m,
                  "Metal: '%s' declares parameter '%s' in %s memory and is "
                  "called with %s pointer",
                  fn_name(fn), source_name(var->name),
                  var->space == MS_TG ? "workgroup"
                  : var->space == MS_THREAD ? "private"
                                            : "global",
                  space_name(given));
      } else if (given != MS_NONE) {
        var->space = given;
      }
    }
  }
}

static void register_declarations(MslFn *fn) {
  IRFunction *func = fn->func;
  for (size_t i = 0; i < func->instruction_count; i++) {
    const IRInstruction *in = &func->instructions[i];
    if (in->op == IR_OP_DECLARE_LOCAL && in->dest.name) {
      declare_var(fn, in->dest.name, in->value_type, in->text,
                  in->dest.kind != IR_OPERAND_TEMP);
    } else if (in->op == IR_OP_ADDRESS_SPACE_ALLOC && in->dest.name) {
      MslVar *var = add_var(fn, in->dest.name, 1);
      int changed = 0;
      if (!var) {
        continue;
      }
      var->fixed = 1;
      var->cls = MC_PTR;
      var->alloc = in->rhs.kind == IR_OPERAND_INT && in->rhs.int_value == 0
                       ? 2
                       : 1;
      var->alloc_count = in->rhs.kind == IR_OPERAND_INT ? in->rhs.int_value : 0;
      var->alloc_space = space_of_mtlc(in->address_space);
      if (in->value_type && in->value_type->kind == MTLC_TYPE_POINTER &&
          in->value_type->base_type) {
        var->alloc_elem_type = in->value_type->base_type;
        var->alloc_elem = type_kind(in->value_type->base_type, NULL);
        var->elem = var->alloc_elem;
      }
      join_space(var, var->alloc_space, &changed);
      if (var->alloc == 2) {
        fn->has_dynamic = 1;
      }
    }
  }
}

static void check_spaces(MslFn *fn) {
  for (size_t i = 0; i < fn->nvars && !fn->m->error; i++) {
    MslVar *var = &fn->vars[i];
    if (var->cls != MC_PTR) {
      continue;
    }
    if (var->space == MS_CONFLICT) {
      mod_error(fn->m,
                "Metal has no generic address space: '%s' in '%s' holds %s "
                "address on one path and another kind on another; keep "
                "workgroup, private and global pointers in separate "
                "variables, or give the helper one space per call",
                source_name(var->name), fn_name(fn),
                space_name(var->conflict_with));
    }
  }
}

static int operand_names(const IROperand *op, const char *name) {
  return (op->kind == IR_OPERAND_TEMP || op->kind == IR_OPERAND_SYMBOL) &&
         op->name && name && !strcmp(op->name, name);
}

static int literal_exact_f32(const IROperand *op) {
  if (op->kind == IR_OPERAND_FLOAT) {
    float f = (float)op->float_value;
    return op->float_value != op->float_value || (double)f == op->float_value;
  }
  if (op->kind == IR_OPERAND_INT) {
    return op->int_value >= -16777216LL && op->int_value <= 16777216LL;
  }
  return 0;
}

static int f32_exact_operand(MslFn *fn, const IROperand *op) {
  MslVar *var;
  if (op->kind == IR_OPERAND_FLOAT || op->kind == IR_OPERAND_INT) {
    return literal_exact_f32(op);
  }
  var = operand_var(fn, op);
  return var && var->cls == MC_F32;
}

static int is_arith(const char *t) {
  return !strcmp(t, "+") || !strcmp(t, "-") || !strcmp(t, "*") ||
         !strcmp(t, "/");
}

static int rounds_once_operand(MslFn *fn, const IROperand *op) {
  MslVar *var;
  if (op->kind == IR_OPERAND_FLOAT || op->kind == IR_OPERAND_INT) {
    return 1;
  }
  var = operand_var(fn, op);
  return var && var->cls == MC_F32;
}

static int def_is_exact_f32(MslFn *fn, const IRInstruction *in) {
  switch (in->op) {
  case IR_OP_BINARY:
    return in->is_float && in->text && is_arith(in->text) &&
           f32_exact_operand(fn, &in->lhs) && f32_exact_operand(fn, &in->rhs);
  case IR_OP_UNARY:
    return in->is_float && in->text && !strcmp(in->text, "-") &&
           rounds_once_operand(fn, &in->lhs);
  case IR_OP_CAST:
  case IR_OP_ASSIGN:
    return rounds_once_operand(fn, &in->lhs);
  default:
    return 0;
  }
}

static int intrinsic_takes_f32(MtlcIntrinsic intrinsic) {
  switch (intrinsic) {
  case MTLC_INTRINSIC_GPU_F32_TO_F16_BITS:
  case MTLC_INTRINSIC_GPU_F32_TO_BITS:
  case MTLC_INTRINSIC_GPU_F2H2:
  case MTLC_INTRINSIC_GPU_F2BF:
  case MTLC_INTRINSIC_GPU_SUBGROUP_BROADCAST_F32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_ADD_F32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MIN_F32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MAX_F32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_INCLUSIVE_ADD_F32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_EXCLUSIVE_ADD_F32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_SHUFFLE_F32:
    return 1;
  default:
    return is_math_intrinsic(intrinsic);
  }
}

static int use_rounds_to_f32(MslFn *fn, const IRInstruction *in,
                             const char *name) {
  switch (in->op) {
  case IR_OP_CAST: {
    MtlcTypeKind target = cast_kind(in);
    return operand_names(&in->lhs, name) && target == MTLC_TYPE_FLOAT32;
  }
  case IR_OP_STORE:
    if (operand_names(&in->dest, name)) {
      return 0;
    }
    return operand_names(&in->lhs, name) && in->is_float &&
           in->rhs.kind == IR_OPERAND_INT && in->rhs.int_value == 4;
  case IR_OP_ASSIGN: {
    MslVar *dest = in->dest.name ? find_var(fn, in->dest.name) : NULL;
    return dest && dest->cls == MC_F32 && !dest->demoted;
  }
  case IR_OP_RETURN:
    return fn->ret_cls == MC_F32;
  case IR_OP_CALL:
    if (in->intrinsic != MTLC_INTRINSIC_NONE) {
      return intrinsic_takes_f32(in->intrinsic);
    }
    {
      size_t index = 0;
      IRFunction *callee = lookup_function(fn->m, in->text, &index);
      const IRModuleSymbol *symbol =
          callee ? ir_program_lookup_symbol(fn->m->program, callee->name)
                 : NULL;
      if (!symbol || symbol->kind != IR_MODSYM_FUNCTION) {
        return 0;
      }
      for (size_t a = 0; a < in->argument_count; a++) {
        if (operand_names(&in->arguments[a], name) &&
            (a >= symbol->param_count || !symbol->param_types[a] ||
             symbol->param_types[a]->kind != MTLC_TYPE_FLOAT32)) {
          return 0;
        }
      }
      return 1;
    }
  default:
    return 0;
  }
}

static int instruction_reads(const IRInstruction *in, const char *name) {
  if (operand_names(&in->lhs, name) || operand_names(&in->rhs, name)) {
    return 1;
  }
  if (in->op == IR_OP_STORE && operand_names(&in->dest, name)) {
    return 1;
  }
  for (size_t a = 0; a < in->argument_count; a++) {
    if (operand_names(&in->arguments[a], name)) {
      return 1;
    }
  }
  return 0;
}

static int var_demotable(MslFn *fn, MslVar *var) {
  IRFunction *func = fn->func;
  int defs = 0;
  if (var->is_param || var->alloc || var->cls != MC_F64) {
    return 0;
  }
  for (size_t i = 0; i < func->instruction_count; i++) {
    const IRInstruction *in = &func->instructions[i];
    int writes = in->op != IR_OP_STORE && in->op != IR_OP_DECLARE_LOCAL &&
                 operand_names(&in->dest, var->name);
    if (writes) {
      if (!def_is_exact_f32(fn, in)) {
        return 0;
      }
      defs++;
    }
    if (in->op == IR_OP_DECLARE_LOCAL) {
      continue;
    }
    if (in->op == IR_OP_ADDRESS_OF && operand_names(&in->lhs, var->name)) {
      return 0;
    }
    if (instruction_reads(in, var->name) &&
        !use_rounds_to_f32(fn, in, var->name)) {
      return 0;
    }
  }
  return defs > 0;
}

static void demote_float64(MslFn *fn) {
  int changed = 1;
  while (changed) {
    changed = 0;
    for (size_t i = 0; i < fn->nvars; i++) {
      MslVar *var = &fn->vars[i];
      if (var_demotable(fn, var)) {
        var->cls = MC_F32;
        if (var->storage == MTLC_TYPE_FLOAT64) {
          var->storage = MTLC_TYPE_FLOAT32;
        }
        var->demoted = 1;
        changed = 1;
      }
    }
  }
}

static void infer_function(MslFn *fn) {
  IRFunction *func = fn->func;
  int changed = 1;
  int rounds = 0;
  register_signature(fn);
  register_declarations(fn);
  while (changed && rounds < 64) {
    changed = 0;
    rounds++;
    for (size_t i = 0; i < func->instruction_count; i++) {
      infer_instruction(fn, &func->instructions[i], &changed);
    }
  }
  for (size_t i = 0; i < fn->nvars; i++) {
    MslVar *var = &fn->vars[i];
    if (var->cls == MC_NONE) {
      var->cls = MC_I32;
    }
    if (var->cls == MC_PTR && var->space == MS_NONE) {
      var->space = MS_DEVICE;
    }
  }
  demote_float64(fn);
  check_spaces(fn);
}

static void fn_free(MslFn *fn) {
  if (!fn) {
    return;
  }
  for (size_t i = 0; i < fn->nvars; i++) {
    free(fn->vars[i].name);
  }
  free(fn->vars);
  free(fn->blocks);
  free(fn->resident);
  sb_free(&fn->out);
  free(fn);
}

static void spec_name(MslMod *m, MslSpec *spec) {
  IRFunction *func = m->program->functions[spec->function_index];
  const char *name = func->name ? func->name : "fn";
  size_t n = 0;
  int generic = 0;
  char base[200];
  for (const char *p = name; *p && n + 1 < sizeof(base); p++) {
    char c = *p;
    int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
             (c >= '0' && c <= '9') || c == '_';
    base[n++] = ok ? c : '_';
  }
  base[n] = '\0';
  if (func->is_kernel) {
    snprintf(spec->name, sizeof(spec->name), "%s", base);
    return;
  }
  snprintf(spec->name, sizeof(spec->name), "mtl_fn_%s%s", base,
           spec->coherent ? "_c" : "");
  for (size_t p = 0; p < spec->param_count; p++) {
    if (spec->param_space[p] == MS_TG || spec->param_space[p] == MS_THREAD) {
      generic = 1;
    }
  }
  if (generic) {
    size_t len = strlen(spec->name);
    if (len + spec->param_count + 2 < sizeof(spec->name)) {
      spec->name[len++] = '_';
      for (size_t p = 0; p < spec->param_count; p++) {
        spec->name[len++] = spec->param_space[p] == MS_TG       ? 't'
                            : spec->param_space[p] == MS_THREAD ? 'p'
                            : spec->param_space[p] == MS_DEVICE ? 'd'
                                                                : 'v';
      }
      spec->name[len] = '\0';
    }
  }
}

static size_t find_or_add_spec(MslMod *m, size_t function_index,
                               const MslSpace *spaces, size_t count,
                               int coherent) {
  MslSpec *spec;
  for (size_t i = 0; i < m->nspecs; i++) {
    MslSpec *s = &m->specs[i];
    if (s->function_index != function_index || s->param_count != count ||
        s->coherent != coherent) {
      continue;
    }
    if (count == 0 ||
        memcmp(s->param_space, spaces, count * sizeof(MslSpace)) == 0) {
      return i;
    }
  }
  if (m->nspecs == m->capspecs) {
    size_t next = m->capspecs ? m->capspecs * 2 : 16;
    MslSpec *grown = realloc(m->specs, next * sizeof(MslSpec));
    if (!grown) {
      mod_error(m, "Metal: out of memory");
      return SIZE_MAX;
    }
    m->specs = grown;
    m->capspecs = next;
  }
  spec = &m->specs[m->nspecs];
  memset(spec, 0, sizeof(*spec));
  spec->function_index = function_index;
  spec->param_count = count;
  spec->coherent = coherent;
  spec->param_space = calloc(count ? count : 1, sizeof(MslSpace));
  if (!spec->param_space) {
    mod_error(m, "Metal: out of memory");
    return SIZE_MAX;
  }
  if (count) {
    memcpy(spec->param_space, spaces, count * sizeof(MslSpace));
  }
  spec_name(m, spec);
  return m->nspecs++;
}

static MslFn *analyze_spec(MslMod *m, size_t spec_index) {
  MslSpec *spec = &m->specs[spec_index];
  IRFunction *func = m->program->functions[spec->function_index];
  MslFn *fn = calloc(1, sizeof(MslFn));
  const MtlcType *ret;
  if (!fn) {
    mod_error(m, "Metal: out of memory");
    return NULL;
  }
  fn->m = m;
  fn->func = func;
  fn->function_index = spec->function_index;
  fn->symbol = ir_program_lookup_symbol(m->program, func->name);
  fn->spec = spec;
  fn->returns_void = returns_void_of(m, func);
  ret = return_type_of(m, func);
  if (!fn->returns_void) {
    if (type_is_record(ret)) {
      fn->ret_cls = MC_REC;
      fn->ret_rec = ret;
    } else {
      fn->ret_kind = type_kind(ret, func->return_type_name);
      fn->ret_cls = class_of_kind(fn->ret_kind);
    }
  }
  if (!spec->call_spec) {
    spec->instruction_count = func->instruction_count;
    spec->call_spec = calloc(func->instruction_count ? func->instruction_count
                                                     : 1,
                             sizeof(size_t));
    if (!spec->call_spec) {
      mod_error(m, "Metal: out of memory");
      free(fn);
      return NULL;
    }
    for (size_t i = 0; i < func->instruction_count; i++) {
      spec->call_spec[i] = SIZE_MAX;
    }
  }
  infer_function(fn);
  return fn;
}

static int discover_calls(MslMod *m, size_t spec_index) {
  MslSpec *spec = &m->specs[spec_index];
  MslFn *fn = spec->fn;
  IRFunction *func = fn->func;
  int added = 0;
  for (size_t i = 0; i < func->instruction_count && !m->error; i++) {
    const IRInstruction *in = &func->instructions[i];
    size_t callee_index = 0;
    IRFunction *callee;
    MslSpace *spaces;
    size_t target;
    if (in->op != IR_OP_CALL || in->intrinsic != MTLC_INTRINSIC_NONE) {
      continue;
    }
    callee = lookup_function(m, in->text, &callee_index);
    if (!callee) {
      mod_error(m, "Metal: '%s' calls '%s', which has no device definition",
                fn_name(fn), in->text ? in->text : "?");
      return 0;
    }
    if (callee->is_kernel) {
      mod_error(m, "Metal: '%s' calls the kernel '%s' as a function",
                fn_name(fn), callee->name);
      return 0;
    }
    spaces = calloc(callee->parameter_count + 1, sizeof(MslSpace));
    if (!spaces) {
      mod_error(m, "Metal: out of memory");
      return 0;
    }
    for (size_t a = 0; a < in->argument_count && a < callee->parameter_count;
         a++) {
      MslVar *var = operand_var(fn, &in->arguments[a]);
      if (var && var->cls == MC_PTR) {
        spaces[a] = var->space == MS_NONE ? MS_DEVICE : var->space;
      }
    }
    target = find_or_add_spec(m, callee_index, spaces,
                              callee->parameter_count,
                              m->specs[spec_index].coherent);
    free(spaces);
    if (target == SIZE_MAX) {
      return 0;
    }
    spec = &m->specs[spec_index];
    if (spec->call_spec[i] != target) {
      spec->call_spec[i] = target;
      added = 1;
    }
  }
  return added;
}

static int analyze_all(MslMod *m) {
  int rounds = 0;
  int changed = 1;
  while (changed && !m->error && rounds < 32) {
    changed = 0;
    rounds++;
    for (size_t s = 0; s < m->nspecs && !m->error; s++) {
      MslFn *fn;
      MslSpace ret_space = MS_NONE;
      fn_free(m->specs[s].fn);
      m->specs[s].fn = NULL;
      fn = analyze_spec(m, s);
      if (!fn) {
        return 0;
      }
      m->specs[s].fn = fn;
      if (discover_calls(m, s)) {
        changed = 1;
      }
      if (fn->ret_cls == MC_PTR) {
        for (size_t i = 0; i < fn->func->instruction_count; i++) {
          const IRInstruction *in = &fn->func->instructions[i];
          if (in->op == IR_OP_RETURN) {
            MslSpace space = operand_space(fn, &in->lhs);
            if (space != MS_NONE) {
              ret_space = ret_space == MS_NONE || ret_space == space
                              ? space
                              : MS_CONFLICT;
            }
          }
        }
        if (ret_space == MS_CONFLICT) {
          mod_error(m,
                    "Metal has no generic address space: '%s' returns "
                    "pointers into different memories",
                    fn_name(fn));
          return 0;
        }
        if (m->specs[s].ret_space != ret_space) {
          m->specs[s].ret_space = ret_space;
          changed = 1;
        }
      }
    }
  }
  return !m->error;
}

static unsigned direct_builtins(MslFn *fn) {
  unsigned mask = 0;
  for (size_t i = 0; i < fn->func->instruction_count; i++) {
    const IRInstruction *in = &fn->func->instructions[i];
    unsigned bit;
    const char *base;
    if (in->op == IR_OP_TENSOR_MMA || in->op == IR_OP_TENSOR_SCALE_ROWS) {
      mask |= MB_LANE | MB_LANES;
      continue;
    }
    if (in->op != IR_OP_CALL || in->intrinsic == MTLC_INTRINSIC_NONE) {
      continue;
    }
    if (builtin_component(in->intrinsic, &bit, &base) >= 0) {
      mask |= bit;
    } else if (in->intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_LOCAL_ID) {
      mask |= MB_LANE;
    } else if (in->intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_SIZE ||
               in->intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_BALLOT_WORD) {
      mask |= MB_LANES;
    }
    if (in->intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_SHUFFLE_U32 ||
        in->intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_SHUFFLE_F32) {
      mask |= MB_LANE | MB_LANES;
    }
  }
  return mask;
}

static void compute_builtins(MslMod *m) {
  int changed = 1;
  for (size_t s = 0; s < m->nspecs; s++) {
    m->specs[s].builtins = direct_builtins(m->specs[s].fn);
  }
  while (changed) {
    changed = 0;
    for (size_t s = 0; s < m->nspecs; s++) {
      MslSpec *spec = &m->specs[s];
      for (size_t i = 0; i < spec->instruction_count; i++) {
        size_t target = spec->call_spec[i];
        unsigned merged;
        if (target == SIZE_MAX) {
          continue;
        }
        merged = spec->builtins | m->specs[target].builtins;
        if (merged != spec->builtins) {
          spec->builtins = merged;
          changed = 1;
        }
      }
    }
  }
}

static const char *record_name(MslMod *m, const MtlcType *type) {
  size_t size = mtlc_type_size(type);
  size_t align = mtlc_type_alignment(type);
  char name[160];
  if (align == 0) {
    align = 1;
  }
  for (size_t i = 0; i < m->nrecords; i++) {
    const MtlcType *other = m->records[i].type;
    if (other == type ||
        (mtlc_type_size(other) == size &&
         (mtlc_type_alignment(other) ? mtlc_type_alignment(other) : 1) ==
             align &&
         other->name && type->name && !strcmp(other->name, type->name))) {
      return m->records[i].name;
    }
  }
  if (type->name && type->name[0]) {
    size_t n = 0;
    char base[96];
    for (const char *p = type->name; *p && n + 1 < sizeof(base); p++) {
      char c = *p;
      int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_';
      base[n++] = ok ? c : '_';
    }
    base[n] = '\0';
    snprintf(name, sizeof(name), "mtl_rec_%s_%zu", base, m->nrecords);
  } else {
    snprintf(name, sizeof(name), "mtl_rec_%zu_%zu_%zu", size, align,
             m->nrecords);
  }
  if (m->nrecords == m->caprecords) {
    size_t next = m->caprecords ? m->caprecords * 2 : 8;
    MslRecord *grown = realloc(m->records, next * sizeof(MslRecord));
    if (!grown) {
      mod_error(m, "Metal: out of memory");
      return "mtl_rec_error";
    }
    m->records = grown;
    m->caprecords = next;
  }
  m->records[m->nrecords].type = type;
  snprintf(m->records[m->nrecords].name, sizeof(m->records[m->nrecords].name),
           "%s", name);
  sb_printf(&m->records_out, "struct alignas(%zu) %s {\n  uchar bytes[%zu];\n};\n",
            align, name, size ? size : 1);
  sb_printf(&m->records_out,
            "static_assert(sizeof(%s) == %zu, \"record layout\");\n\n", name,
            size ? size : 1);
  return m->records[m->nrecords++].name;
}

static void indent(MslFn *fn) {
  for (int i = 0; i < fn->indent; i++) {
    sb_puts(&fn->out, "  ");
  }
}

static void line(MslFn *fn, const char *format, ...) {
  char buffer[4096];
  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  indent(fn);
  sb_puts(&fn->out, buffer);
  sb_puts(&fn->out, "\n");
  if (fn->reported) {
    ir_explain_device_code(
        fn->func->name,
        fn->m->current ? ir_device_report_code_line(fn->func, fn->m->current)
                       : fn->func->location.line,
        buffer);
  }
}

static void report_note(MslFn *fn, const IRInstruction *in, const char *kind,
                        const char *subject, IRDeviceVerdict verdict,
                        const char *headline, const char *reason,
                        const char *code) {
  if (!fn->reported) {
    return;
  }
  ir_explain_device_note_site(fn->func->name,
                              in ? in->location.line : fn->func->location.line,
                              kind, subject, verdict, headline, reason, NULL,
                              code);
}

static void report_whole_value(MslFn *fn, const IRInstruction *in,
                               const char *what, const char *kept,
                               size_t bytes) {
  char shown[128], subject[176], headline[96], cause[300], reason[380];
  int has_reason;
  ir_display_name(in->dest.name, shown, sizeof(shown));
  snprintf(subject, sizeof(subject), "%s `%s`", what, shown);
  snprintf(headline, sizeof(headline), "kept as one %s, %zu bytes", kept,
           bytes);
  has_reason = ir_device_report_storage_reason(fn->func, in->dest.name, 1,
                                               cause, sizeof(cause));
  snprintf(reason, sizeof(reason), "%s, so it is not split into scalars",
           cause);
  ir_explain_device_add(fn->func->name, IR_DEVICE_VALUES_IN_MEMORY, 1);
  ir_explain_device_add(fn->func->name, IR_DEVICE_MEMORY_BYTES,
                        (long long)bytes);
  ir_explain_device_note(fn->func->name, in->location.line, "storage", subject,
                         has_reason ? IR_DEVICE_MISS : IR_DEVICE_FACT, headline,
                         has_reason ? reason : NULL, NULL, "local-memory");
}

static int report_declared_before(const IRFunction *func, size_t index) {
  const char *name = func->instructions[index].dest.name;
  for (size_t i = 0; i < index; i++) {
    const IRInstruction *in = &func->instructions[i];
    if (in->op == IR_OP_DECLARE_LOCAL && in->dest.name &&
        strcmp(in->dest.name, name) == 0) {
      return 1;
    }
  }
  return 0;
}

static void report_allocation(MslFn *fn, const IRInstruction *in) {
  const char *name = fn->func->name;
  size_t element = in->value_type && in->value_type->base_type
                       ? mtlc_type_size(in->value_type->base_type)
                       : 0;
  long long count = in->rhs.kind == IR_OPERAND_INT ? in->rhs.int_value : 0;
  if (in->address_space != MTLC_ADDRESS_SPACE_WORKGROUP) {
    report_whole_value(fn, in, "private array", "thread array",
                       element * (size_t)count);
  } else if (count == 0) {
    ir_explain_device_set(name, IR_DEVICE_WORKGROUP_LAUNCH_SIZED, 1);
  } else {
    ir_explain_device_add(name, IR_DEVICE_WORKGROUP_BYTES,
                          (long long)element * count);
  }
}

static void report_instruction(MslFn *fn, size_t index) {
  IRFunction *func = fn->func;
  const IRInstruction *in = &func->instructions[index];
  const char *name = func->name;
  switch (in->op) {
  case IR_OP_DECLARE_LOCAL:
    if (!in->dest.name || report_declared_before(func, index)) {
      return;
    }
    if (type_is_record(in->value_type)) {
      int array = in->value_type->kind == MTLC_TYPE_ARRAY;
      report_whole_value(fn, in, array ? "array" : "record",
                         array ? "array" : "record",
                         mtlc_type_size(in->value_type));
    } else {
      ir_explain_device_add(name, IR_DEVICE_VALUES_IN_REGISTERS, 1);
    }
    return;
  case IR_OP_ADDRESS_SPACE_ALLOC:
    if (in->dest.name) {
      report_allocation(fn, in);
    }
    return;
  case IR_OP_LOAD:
    if (in->lhs.kind != IR_OPERAND_STRING) {
      ir_explain_device_add(name, IR_DEVICE_ACCESSES_NAMED, 1);
    }
    return;
  case IR_OP_STORE:
    ir_explain_device_add(name, IR_DEVICE_ACCESSES_NAMED, 1);
    return;
  case IR_OP_CALL:
    if (in->intrinsic == MTLC_INTRINSIC_NONE &&
        lookup_function(fn->m, in->text, NULL)) {
      ir_explain_device_add(name, IR_DEVICE_CALLS_KEPT, 1);
    }
    return;
  default:
    return;
  }
}

static void report_function(MslFn *fn) {
  IRFunction *func = fn->func;
  size_t before = ir_explain_device_function_count();
  fn->reported = 0;
  fn->m->current = NULL;
  if (!ir_explain_device_function(func->name, func->location.filename,
                                  func->location.line, func->is_kernel,
                                  func->kernel_block) ||
      ir_explain_device_function_count() == before) {
    return;
  }
  fn->reported = 1;
  for (size_t p = 0; p < func->parameter_count; p++) {
    MslVar *var = func->parameter_names && func->parameter_names[p]
                      ? find_var(fn, func->parameter_names[p])
                      : NULL;
    if (var && var->cls == MC_REC) {
      ir_explain_device_add(func->name, IR_DEVICE_VALUES_IN_MEMORY, 1);
      ir_explain_device_add(func->name, IR_DEVICE_MEMORY_BYTES,
                            (long long)mtlc_type_size(var->rec));
    } else {
      ir_explain_device_add(func->name, IR_DEVICE_VALUES_IN_REGISTERS, 1);
    }
  }
  for (size_t i = 0; i < func->instruction_count; i++) {
    report_instruction(fn, i);
  }
  for (size_t i = 0; i < fn->nvars; i++) {
    if (fn->vars[i].demoted) {
      ir_explain_device_note_site(
          func->name, func->location.line, "numeric", "float64 values",
          IR_DEVICE_GOOD, "computed in float32 with the same bits",
          "every operand is a float32 value or a literal float32 holds "
          "exactly, and the result is only rounded back to float32",
          NULL, "float64-narrowed");
    }
  }
}

static void print_float(char *out, size_t size, double value) {
  float f = (float)value;
  uint32_t bits;
  memcpy(&bits, &f, 4);
  if (f != f || f > 3.4028235e38f || f < -3.4028235e38f) {
    snprintf(out, size, "as_type<float>(0x%08xu)", bits);
    return;
  }
  {
    char digits[48];
    snprintf(digits, sizeof(digits), "%.9g", (double)f);
    snprintf(out, size, "%s%s%s%sf%s", digits[0] == '-' ? "(" : "", digits,
             (strchr(digits, '.') || strchr(digits, 'e')) ? "" : ".0",
             "", digits[0] == '-' ? ")" : "");
  }
}

static void print_int_literal(char *out, size_t size, long long value,
                              MslClass cls) {
  switch (cls) {
  case MC_U32:
    snprintf(out, size, "%uu", (unsigned)(uint32_t)value);
    return;
  case MC_U64:
    snprintf(out, size, "%lluul", (unsigned long long)value);
    return;
  case MC_I64:
    if (value == INT64_MIN) {
      snprintf(out, size, "(-9223372036854775807l - 1l)");
    } else if (value < 0) {
      snprintf(out, size, "(%lldl)", value);
    } else {
      snprintf(out, size, "%lldl", value);
    }
    return;
  default: {
    int32_t v = (int32_t)(uint32_t)(uint64_t)value;
    if (v == INT32_MIN) {
      snprintf(out, size, "(-2147483647 - 1)");
    } else if (v < 0) {
      snprintf(out, size, "(%d)", v);
    } else {
      snprintf(out, size, "%d", v);
    }
    return;
  }
  }
}

static void convert_expr(MslFn *fn, char *out, size_t size, const char *expr,
                         MslClass from, MslClass to) {
  (void)fn;
  if (from == to || to == MC_NONE || from == MC_NONE) {
    snprintf(out, size, "%s", expr);
    return;
  }
  if (to == MC_I32 && class_is_int(from)) {
    if (from == MC_U32) {
      snprintf(out, size, "as_type<int>(%s)", expr);
    } else {
      snprintf(out, size, "as_type<int>((uint)(%s))", expr);
    }
    return;
  }
  if (to == MC_I64 && from == MC_U64) {
    snprintf(out, size, "as_type<long>(%s)", expr);
    return;
  }
  if (to == MC_PTR || from == MC_PTR) {
    if (to == MC_PTR) {
      snprintf(out, size, "reinterpret_cast<device uchar*>((ulong)(%s))",
               expr);
    } else {
      snprintf(out, size, "(%s)reinterpret_cast<ulong>(%s)", class_type(to),
               expr);
    }
    return;
  }
  snprintf(out, size, "(%s)(%s)", class_type(to), expr);
}

static void var_read(MslVar *var, char *out, size_t size) {
  MslClass natural;
  if (var->cls == MC_PTR || var->cls == MC_REC ||
      var->storage == MTLC_TYPE_VOID) {
    snprintf(out, size, "%s", var->ident);
    return;
  }
  natural = class_of_kind(var->storage);
  switch (var->storage) {
  case MTLC_TYPE_INT8:
  case MTLC_TYPE_INT16:
    snprintf(out, size, "(int)%s", var->ident);
    return;
  case MTLC_TYPE_UINT8:
  case MTLC_TYPE_UINT16:
  case MTLC_TYPE_BOOL:
    snprintf(out, size, "(uint)%s", var->ident);
    return;
  case MTLC_TYPE_FLOAT16:
  case MTLC_TYPE_BFLOAT16:
    snprintf(out, size, "(float)%s", var->ident);
    return;
  default:
    (void)natural;
    snprintf(out, size, "%s", var->ident);
    return;
  }
}

static void materialize(MslFn *fn, const IROperand *op, MslClass want,
                        char *out, size_t size) {
  MslVar *var;
  char tmp[2048];
  if (op->kind == IR_OPERAND_INT) {
    if (want == MC_F32 || want == MC_F64) {
      print_float(out, size, (double)op->int_value);
    } else if (want == MC_PTR) {
      if (op->int_value == 0) {
        snprintf(out, size, "nullptr");
      } else {
        snprintf(out, size, "reinterpret_cast<device uchar*>(%lluul)",
                 (unsigned long long)op->int_value);
      }
    } else {
      print_int_literal(out, size, op->int_value,
                        want == MC_NONE ? literal_class(op) : want);
    }
    return;
  }
  if (op->kind == IR_OPERAND_FLOAT) {
    if (want == MC_F32 || want == MC_F64 || want == MC_NONE) {
      print_float(out, size, op->float_value);
    } else {
      double v = op->float_value;
      long long iv = (v != v) ? 0 : (long long)v;
      print_int_literal(out, size, iv, want);
    }
    return;
  }
  var = operand_var(fn, op);
  if (!var) {
    mod_error(fn->m, "Metal: '%s' reads '%s' before anything defines it",
              fn_name(fn), source_name(op->name));
    snprintf(out, size, "0");
    return;
  }
  var_read(var, tmp, sizeof(tmp));
  if (var->cls == MC_PTR && want == MC_PTR) {
    snprintf(out, size, "%s", tmp);
    return;
  }
  if (var->cls == MC_REC) {
    snprintf(out, size, "%s", tmp);
    return;
  }
  convert_expr(fn, out, size, tmp, var->cls, want);
}

static void store_to_var(MslFn *fn, MslVar *var, const char *expr,
                         MslClass expr_class) {
  char converted[4096];
  if (var->cls == MC_REC || var->cls == MC_PTR) {
    line(fn, "%s = %s;", var->ident, expr);
    return;
  }
  convert_expr(fn, converted, sizeof(converted), expr, expr_class, var->cls);
  switch (var->storage) {
  case MTLC_TYPE_INT8:
  case MTLC_TYPE_UINT8:
  case MTLC_TYPE_INT16:
  case MTLC_TYPE_UINT16:
  case MTLC_TYPE_FLOAT16:
  case MTLC_TYPE_BFLOAT16:
    line(fn, "%s = (%s)(%s);", var->ident, storage_type(var->storage),
         converted);
    return;
  case MTLC_TYPE_BOOL:
    line(fn, "%s = (uchar)((%s) != 0u);", var->ident, converted);
    return;
  default:
    line(fn, "%s = %s;", var->ident, converted);
    return;
  }
}

static MslVar *dest_var(MslFn *fn, const IRInstruction *in) {
  if (!in->dest.name) {
    return NULL;
  }
  return find_var(fn, in->dest.name);
}

static void store_dest(MslFn *fn, const IRInstruction *in, const char *expr,
                       MslClass expr_class) {
  MslVar *var = dest_var(fn, in);
  if (!var) {
    return;
  }
  store_to_var(fn, var, expr, expr_class);
}

static void float64_refusal(MslFn *fn, const IRInstruction *in) {
  fn->m->current = in;
  mod_error(fn->m,
            "Metal has no 64-bit float: '%s' computes in float64 here, "
            "because a fractional literal like 0.5 is float64 and pulls the "
            "arithmetic with it, and Apple GPUs have no double. Write the "
            "literal as (float32)0.5 or keep the value in float32",
            fn_name(fn));
}

static uint32_t bits_of_f32(float f) {
  uint32_t bits;
  memcpy(&bits, &f, 4);
  return bits;
}

static float f32_of_bits(uint32_t bits) {
  float f;
  memcpy(&f, &bits, 4);
  return f;
}

static float f32_next_up(float f) {
  uint32_t bits = bits_of_f32(f);
  if (f != f || bits == 0x7f800000u) {
    return f;
  }
  if ((bits & 0x7fffffffu) == 0) {
    return f32_of_bits(1u);
  }
  return f32_of_bits((bits & 0x80000000u) ? bits - 1u : bits + 1u);
}

static float f32_next_down(float f) { return -f32_next_up(-f); }

static const char *mirror_compare(const char *t) {
  if (!strcmp(t, "<")) return ">";
  if (!strcmp(t, ">")) return "<";
  if (!strcmp(t, "<=")) return ">=";
  if (!strcmp(t, ">=")) return "<=";
  return t;
}

static void emit_f64_compare(MslFn *fn, const IRInstruction *in,
                             const char *t) {
  int left_literal = in->lhs.kind == IR_OPERAND_FLOAT ||
                     in->lhs.kind == IR_OPERAND_INT;
  int right_literal = in->rhs.kind == IR_OPERAND_FLOAT ||
                      in->rhs.kind == IR_OPERAND_INT;
  char x[2048], expr[2400], bound[64];
  const IROperand *variable;
  const IROperand *literal;
  double value;
  float nearest;
  if (left_literal && right_literal) {
    double l = in->lhs.kind == IR_OPERAND_FLOAT ? in->lhs.float_value
                                                : (double)in->lhs.int_value;
    double r = in->rhs.kind == IR_OPERAND_FLOAT ? in->rhs.float_value
                                                : (double)in->rhs.int_value;
    int truth = !strcmp(t, "<")    ? l < r
                : !strcmp(t, ">")  ? l > r
                : !strcmp(t, "<=") ? l <= r
                : !strcmp(t, ">=") ? l >= r
                : !strcmp(t, "==") ? l == r
                                   : l != r;
    store_dest(fn, in, truth ? "1u" : "0u", MC_U32);
    return;
  }
  if (!left_literal && !right_literal) {
    char y[2048];
    materialize(fn, &in->lhs, MC_F32, x, sizeof(x));
    materialize(fn, &in->rhs, MC_F32, y, sizeof(y));
    snprintf(expr, sizeof(expr), "(uint)(%s %s %s)", x, t, y);
    store_dest(fn, in, expr, MC_U32);
    return;
  }
  variable = left_literal ? &in->rhs : &in->lhs;
  literal = left_literal ? &in->lhs : &in->rhs;
  if (left_literal) {
    t = mirror_compare(t);
  }
  value = literal->kind == IR_OPERAND_FLOAT ? literal->float_value
                                            : (double)literal->int_value;
  materialize(fn, variable, MC_F32, x, sizeof(x));
  if (value != value) {
    store_dest(fn, in, !strcmp(t, "!=") ? "1u" : "0u", MC_U32);
    return;
  }
  nearest = (float)value;
  if ((double)nearest == value) {
    print_float(bound, sizeof(bound), (double)nearest);
    snprintf(expr, sizeof(expr), "(uint)(%s %s %s)", x, t, bound);
    store_dest(fn, in, expr, MC_U32);
    return;
  }
  {
    float lo = (double)nearest > value ? f32_next_down(nearest) : nearest;
    float hi = (double)nearest > value ? nearest : f32_next_up(nearest);
    if (!strcmp(t, "==")) {
      store_dest(fn, in, "0u", MC_U32);
      return;
    }
    if (!strcmp(t, "!=")) {
      store_dest(fn, in, "1u", MC_U32);
      return;
    }
    if (!strcmp(t, ">") || !strcmp(t, ">=")) {
      print_float(bound, sizeof(bound), (double)hi);
      snprintf(expr, sizeof(expr), "(uint)(%s >= %s)", x, bound);
    } else {
      print_float(bound, sizeof(bound), (double)lo);
      snprintf(expr, sizeof(expr), "(uint)(%s <= %s)", x, bound);
    }
    store_dest(fn, in, expr, MC_U32);
  }
}

static int check_f64(MslFn *fn, MslClass cls) {
  if (cls == MC_F64) {
    mod_error(fn->m,
              "Metal has no 64-bit float: '%s' holds a float64 value, and "
              "Apple GPUs have no double. A fractional literal like 0.5 is "
              "float64; write (float32)0.5 or keep the value in float32",
              fn_name(fn));
    return 0;
  }
  return 1;
}

static int emit_binary_f64(MslFn *fn, const IRInstruction *in, const char *t,
                           MslClass a, MslClass b, MslVar *dest) {
  if (is_compare(t) && strcmp(t, "&&") != 0 && strcmp(t, "||") != 0 &&
      a != MC_F64 && b != MC_F64 &&
      (a == MC_F32 || in->lhs.kind == IR_OPERAND_FLOAT ||
       in->lhs.kind == IR_OPERAND_INT) &&
      (b == MC_F32 || in->rhs.kind == IR_OPERAND_FLOAT ||
       in->rhs.kind == IR_OPERAND_INT)) {
    emit_f64_compare(fn, in, t);
    return 1;
  }
  if (!(is_arith(t) && dest->cls == MC_F32 && dest->demoted &&
        f32_exact_operand(fn, &in->lhs) &&
        f32_exact_operand(fn, &in->rhs))) {
    float64_refusal(fn, in);
    return 1;
  }
  return 0;
}

static void emit_binary_float(MslFn *fn, const IRInstruction *in,
                              const char *t, MslClass a, MslClass b) {
  char x[2048], y[2048], expr[4600];
  if (a == MC_F64 || b == MC_F64) {
    float64_refusal(fn, in);
    return;
  }
  materialize(fn, &in->lhs, MC_F32, x, sizeof(x));
  materialize(fn, &in->rhs, MC_F32, y, sizeof(y));
  if (is_compare(t)) {
    if (!strcmp(t, "&&") || !strcmp(t, "||")) {
      snprintf(expr, sizeof(expr), "(uint)((%s != 0.0f) %s (%s != 0.0f))",
               x, t, y);
    } else {
      snprintf(expr, sizeof(expr), "(uint)(%s %s %s)", x, t, y);
    }
    store_dest(fn, in, expr, MC_U32);
    return;
  }
  if (!strcmp(t, "%")) {
    snprintf(expr, sizeof(expr), "fmod(%s, %s)", x, y);
  } else if (!strcmp(t, "+") || !strcmp(t, "-") || !strcmp(t, "*") ||
             !strcmp(t, "/")) {
    snprintf(expr, sizeof(expr), "%s %s %s", x, t, y);
  } else {
    mod_error(fn->m, "Metal: unsupported float operator '%s' in '%s'", t,
              fn_name(fn));
    return;
  }
  store_dest(fn, in, expr, MC_F32);
}

static void emit_binary_two_pointers(MslFn *fn, const IRInstruction *in,
                                     const char *t) {
  char x[2048], y[2048], expr[4600];
  materialize(fn, &in->lhs, MC_PTR, x, sizeof(x));
  materialize(fn, &in->rhs, MC_PTR, y, sizeof(y));
  if (!strcmp(t, "-")) {
    snprintf(expr, sizeof(expr), "(long)(%s - %s)", x, y);
    store_dest(fn, in, expr, MC_I64);
  } else if (is_compare(t)) {
    snprintf(expr, sizeof(expr), "(uint)(%s %s %s)", x, t, y);
    store_dest(fn, in, expr, MC_U32);
  } else {
    mod_error(fn->m, "Metal: '%s' applies '%s' to two pointers",
              fn_name(fn), t);
  }
}

static void emit_binary_pointer(MslFn *fn, const IRInstruction *in,
                                const char *t, MslClass a, MslClass b,
                                MslVar *dest) {
  char x[2048], y[2048], expr[4600];
  if (a == MC_PTR && b == MC_PTR) {
    emit_binary_two_pointers(fn, in, t);
    return;
  }
  if (is_compare(t)) {
    materialize(fn, &in->lhs, a == MC_PTR ? MC_PTR : MC_U64, x, sizeof(x));
    materialize(fn, &in->rhs, b == MC_PTR ? MC_PTR : MC_U64, y, sizeof(y));
    if (in->lhs.kind == IR_OPERAND_INT && in->lhs.int_value == 0) {
      snprintf(x, sizeof(x), "nullptr");
    }
    if (in->rhs.kind == IR_OPERAND_INT && in->rhs.int_value == 0) {
      snprintf(y, sizeof(y), "nullptr");
    }
    snprintf(expr, sizeof(expr), "(uint)(%s %s %s)", x, t, y);
    store_dest(fn, in, expr, MC_U32);
    return;
  }
  if (strcmp(t, "+") != 0 && strcmp(t, "-") != 0) {
    mod_error(fn->m,
              "Metal: '%s' applies '%s' to a pointer; only + and - move "
              "a pointer",
              fn_name(fn), t);
    return;
  }
  if (a == MC_PTR) {
    materialize(fn, &in->lhs, MC_PTR, x, sizeof(x));
    materialize(fn, &in->rhs, MC_I64, y, sizeof(y));
  } else {
    materialize(fn, &in->rhs, MC_PTR, x, sizeof(x));
    materialize(fn, &in->lhs, MC_I64, y, sizeof(y));
  }
  snprintf(expr, sizeof(expr), "%s %s %s", x, t, y);
  if (dest->cls == MC_PTR) {
    line(fn, "%s = %s;", dest->ident, expr);
  } else {
    char wrapped[4700];
    snprintf(wrapped, sizeof(wrapped), "(%s)", expr);
    store_dest(fn, in, wrapped, MC_PTR);
  }
}

static void emit_binary_shift(MslFn *fn, const IRInstruction *in,
                              const char *t, int wide, const char *x,
                              const char *y, const char *sx) {
  MslClass op_cls = wide ? MC_U64 : MC_U32;
  MslClass signed_cls = wide ? MC_I64 : MC_I32;
  const char *mask = wide ? "63ul" : "31u";
  int uns = in->is_unsigned;
  char expr[4600], count[2100];
  if (in->rhs.kind == IR_OPERAND_INT) {
    snprintf(count, sizeof(count), "%lluu",
             (unsigned long long)in->rhs.int_value & (wide ? 63ull : 31ull));
  } else {
    snprintf(count, sizeof(count), "(%s & %s)", y, mask);
  }
  if (!strcmp(t, "<<")) {
    snprintf(expr, sizeof(expr), "%s << %s", x, count);
    store_dest(fn, in, expr, op_cls);
  } else if (uns) {
    snprintf(expr, sizeof(expr), "%s >> %s", x, count);
    store_dest(fn, in, expr, op_cls);
  } else {
    snprintf(expr, sizeof(expr), "%s >> %s", sx, count);
    store_dest(fn, in, expr, signed_cls);
  }
}

static void emit_binary_int(MslFn *fn, const IRInstruction *in, const char *t,
                            MslClass a, MslClass b) {
  int wide = class_is_64(a) || class_is_64(b);
  MslClass op_cls = wide ? MC_U64 : MC_U32;
  MslClass signed_cls = wide ? MC_I64 : MC_I32;
  const char *ut = unsigned_type(op_cls);
  const char *st = signed_type(op_cls);
  int uns = in->is_unsigned;
  char x[2048], y[2048], expr[4600];
  char sx[2048], sy[2048];
  materialize(fn, &in->lhs, op_cls, x, sizeof(x));
  materialize(fn, &in->rhs, op_cls, y, sizeof(y));
  materialize(fn, &in->lhs, signed_cls, sx, sizeof(sx));
  materialize(fn, &in->rhs, signed_cls, sy, sizeof(sy));
  if (!strcmp(t, "+") || !strcmp(t, "-") || !strcmp(t, "*") ||
      !strcmp(t, "&") || !strcmp(t, "|") || !strcmp(t, "^")) {
    snprintf(expr, sizeof(expr), "%s %s %s", x, t, y);
    store_dest(fn, in, expr, op_cls);
  } else if (!strcmp(t, "<<") || !strcmp(t, ">>")) {
    emit_binary_shift(fn, in, t, wide, x, y, sx);
  } else if (!strcmp(t, "/")) {
    if (uns) {
      snprintf(expr, sizeof(expr), "%s / %s", x, y);
      store_dest(fn, in, expr, op_cls);
    } else {
      snprintf(expr, sizeof(expr),
               "(%s == (%s)(-1)) ? (%s)((%s)0 - %s) : (%s)(%s / %s)", sy,
               st, ut, ut, x, ut, sx, sy);
      store_dest(fn, in, expr, op_cls);
    }
  } else if (!strcmp(t, "%")) {
    if (uns) {
      snprintf(expr, sizeof(expr), "%s %% %s", x, y);
      store_dest(fn, in, expr, op_cls);
    } else {
      snprintf(expr, sizeof(expr),
               "(%s == (%s)(-1)) ? (%s)0 : (%s - (%s)(%s / %s) * %s)", sy,
               st, ut, x, ut, sx, sy, y);
      store_dest(fn, in, expr, op_cls);
    }
  } else if (!strcmp(t, "&&") || !strcmp(t, "||")) {
    snprintf(expr, sizeof(expr), "(uint)((%s != 0) %s (%s != 0))", x, t, y);
    store_dest(fn, in, expr, MC_U32);
  } else if (is_compare(t)) {
    if (uns || !strcmp(t, "==") || !strcmp(t, "!=")) {
      snprintf(expr, sizeof(expr), "(uint)(%s %s %s)", x, t, y);
    } else {
      snprintf(expr, sizeof(expr), "(uint)(%s %s %s)", sx, t, sy);
    }
    store_dest(fn, in, expr, MC_U32);
  } else {
    mod_error(fn->m, "Metal: unsupported operator '%s' in '%s'", t,
              fn_name(fn));
  }
}

static void emit_binary(MslFn *fn, const IRInstruction *in) {
  const char *t = in->text ? in->text : "+";
  MslClass a = operand_class(fn, &in->lhs);
  MslClass b = operand_class(fn, &in->rhs);
  MslVar *dest = dest_var(fn, in);
  if (!dest) {
    return;
  }
  if (in->is_float && in->float_bits == 64 &&
      emit_binary_f64(fn, in, t, a, b, dest)) {
    return;
  }
  if (in->is_float || a == MC_F32 || b == MC_F32) {
    emit_binary_float(fn, in, t, a, b);
    return;
  }
  if (a == MC_PTR || b == MC_PTR) {
    emit_binary_pointer(fn, in, t, a, b, dest);
    return;
  }
  emit_binary_int(fn, in, t, a, b);
}

static void emit_unary(MslFn *fn, const IRInstruction *in) {
  const char *t = in->text ? in->text : "";
  MslClass a = operand_class(fn, &in->lhs);
  char x[2048], expr[2200];
  if (!strcmp(t, "!")) {
    if (a == MC_F32 || in->is_float) {
      materialize(fn, &in->lhs, MC_F32, x, sizeof(x));
      snprintf(expr, sizeof(expr), "(uint)(%s == 0.0f)", x);
    } else if (a == MC_PTR) {
      materialize(fn, &in->lhs, MC_PTR, x, sizeof(x));
      snprintf(expr, sizeof(expr), "(uint)(%s == nullptr)", x);
    } else {
      MslClass c = class_is_64(a) ? MC_U64 : MC_U32;
      materialize(fn, &in->lhs, c, x, sizeof(x));
      snprintf(expr, sizeof(expr), "(uint)(%s == 0)", x);
    }
    store_dest(fn, in, expr, MC_U32);
    return;
  }
  if (in->is_float || a == MC_F32 || a == MC_F64) {
    MslVar *dest = dest_var(fn, in);
    int wide = (in->is_float && in->float_bits == 64) || a == MC_F64;
    if (wide && !(dest && dest->demoted && !strcmp(t, "-") &&
                  rounds_once_operand(fn, &in->lhs))) {
      float64_refusal(fn, in);
      return;
    }
    materialize(fn, &in->lhs, MC_F32, x, sizeof(x));
    if (!strcmp(t, "-")) {
      snprintf(expr, sizeof(expr), "-%s", x);
      store_dest(fn, in, expr, MC_F32);
      return;
    }
    if (!strcmp(t, "sqrt")) {
      snprintf(expr, sizeof(expr), "sqrt(%s)", x);
      store_dest(fn, in, expr, MC_F32);
      return;
    }
    mod_error(fn->m, "Metal: unsupported float unary '%s' in '%s'", t,
              fn_name(fn));
    return;
  }
  {
    MslClass c = class_is_64(a) ? MC_U64 : MC_U32;
    materialize(fn, &in->lhs, c, x, sizeof(x));
    if (!strcmp(t, "-")) {
      snprintf(expr, sizeof(expr), "%s - %s", c == MC_U64 ? "0ul" : "0u", x);
    } else if (!strcmp(t, "~")) {
      snprintf(expr, sizeof(expr), "~%s", x);
    } else if (!strcmp(t, "+")) {
      snprintf(expr, sizeof(expr), "%s", x);
    } else {
      mod_error(fn->m, "Metal: unsupported unary '%s' in '%s'", t,
                fn_name(fn));
      return;
    }
    store_dest(fn, in, expr, c);
  }
}

static const char *access_type(MtlcTypeKind kind) {
  const char *name = storage_type(kind);
  return name ? name : "uint";
}

static void pointer_expr(MslFn *fn, const IROperand *address,
                         MslSpace *space, char *out, size_t size) {
  MslVar *var = operand_var(fn, address);
  *space = var && var->cls == MC_PTR ? var->space : MS_DEVICE;
  materialize(fn, address, MC_PTR, out, size);
}

static void emit_block_copy(MslFn *fn, MslSpace dst_space, const char *dst,
                            MslSpace src_space, const char *src,
                            long long total) {
  long long offset = 0;
  while (offset < total) {
    long long width = total - offset >= 8   ? 8
                      : total - offset >= 4 ? 4
                      : total - offset >= 2 ? 2
                                            : 1;
    const char *type = width == 8 ? "ulong"
                       : width == 4 ? "uint"
                       : width == 2 ? "ushort"
                                    : "uchar";
    line(fn, "*(%s %s*)(%s + %lld) = *(%s %s*)(%s + %lld);",
         space_qual(fn->m, dst_space), type, dst, offset,
         space_qual(fn->m, src_space), type, src, offset);
    offset += width;
  }
}

static void emit_load(MslFn *fn, const IRInstruction *in) {
  MslSpace space;
  char p[2048], expr[2400];
  MtlcTypeKind kind;
  MslVar *dest = dest_var(fn, in);
  if (in->lhs.kind == IR_OPERAND_STRING) {
    if (dest) {
      dest->literal = in->lhs.name;
    }
    return;
  }
  if (!dest) {
    return;
  }
  pointer_expr(fn, &in->lhs, &space, p, sizeof(p));
  if (dest->cls == MC_PTR) {
    line(fn, "%s = *(%s uchar* %s*)(%s);", dest->ident, device_qual(fn->m),
         space_qual(fn->m, space), p);
    return;
  }
  if (dest->cls == MC_REC) {
    long long size = (long long)mtlc_type_size(dest->rec);
    snprintf(expr, sizeof(expr), "*(%s %s*)(%s)", space_qual(fn->m, space),
             record_name(fn->m, dest->rec), p);
    line(fn, "%s = %s;", dest->ident, expr);
    (void)size;
    return;
  }
  kind = access_kind(fn, &in->lhs, in);
  if (!check_f64(fn, class_of_kind(kind))) {
    return;
  }
  snprintf(expr, sizeof(expr), "*(%s %s*)(%s)", space_qual(fn->m, space),
           access_type(kind), p);
  {
    char widened[2600];
    MslClass cls = class_of_kind(kind);
    snprintf(widened, sizeof(widened), "(%s)%s", class_type(cls), expr);
    store_dest(fn, in, widened, cls);
  }
}

static void emit_store(MslFn *fn, const IRInstruction *in) {
  MslSpace space;
  char p[2048], value[2048];
  MtlcTypeKind kind;
  MslVar *source = operand_var(fn, &in->lhs);
  pointer_expr(fn, &in->dest, &space, p, sizeof(p));
  if (source && source->cls == MC_REC) {
    line(fn, "*(%s %s*)(%s) = %s;", space_qual(fn->m, space),
         record_name(fn->m, source->rec), p, source->ident);
    return;
  }
  if (in->rhs.kind == IR_OPERAND_INT && in->rhs.int_value > 8) {
    MslSpace src_space;
    char src[2048];
    pointer_expr(fn, &in->lhs, &src_space, src, sizeof(src));
    emit_block_copy(fn, space, p, src_space, src, in->rhs.int_value);
    return;
  }
  if (source && source->cls == MC_PTR) {
    line(fn, "*(%s uchar* %s*)(%s) = %s;", device_qual(fn->m),
         space_qual(fn->m, space), p, source->ident);
    return;
  }
  kind = access_kind(fn, &in->dest, in);
  if (!check_f64(fn, class_of_kind(kind))) {
    return;
  }
  materialize(fn, &in->lhs, class_of_kind(kind), value, sizeof(value));
  line(fn, "*(%s %s*)(%s) = (%s)(%s);", space_qual(fn->m, space),
       access_type(kind), p, access_type(kind), value);
}

static void emit_cast_to_pointer(MslFn *fn, const IRInstruction *in,
                                 MslVar *dest) {
  char x[2048], expr[2600];
  MslVar *source = operand_var(fn, &in->lhs);
  if (source && source->cls == MC_PTR) {
    if (dest->cls == MC_PTR && dest->space != source->space &&
        source->space != MS_NONE && dest->space != MS_NONE) {
      mod_error(fn->m,
                "Metal: '%s' casts %s pointer '%s' to %s pointer; Metal "
                "cannot convert between address spaces",
                fn_name(fn), space_name(source->space),
                source_name(source->name), space_name(dest->space));
      return;
    }
    materialize(fn, &in->lhs, MC_PTR, x, sizeof(x));
    store_dest(fn, in, x, MC_PTR);
    return;
  }
  if (in->lhs.kind == IR_OPERAND_INT && in->lhs.int_value == 0) {
    store_dest(fn, in, "nullptr", MC_PTR);
    return;
  }
  materialize(fn, &in->lhs, MC_U64, x, sizeof(x));
  snprintf(expr, sizeof(expr), "reinterpret_cast<%s uchar*>(%s)",
           space_qual(fn->m, dest->space), x);
  line(fn, "%s = %s;", dest->ident, expr);
}

static MslClass cast_source(MslFn *fn, const IRInstruction *in,
                            MslClass source_cls, char *x, size_t size) {
  char expr[2600];
  if (source_cls == MC_PTR) {
    materialize(fn, &in->lhs, MC_PTR, x, size);
    snprintf(expr, sizeof(expr), "reinterpret_cast<ulong>(%s)", x);
    snprintf(x, size, "%s", expr);
    return MC_U64;
  }
  if (source_cls == MC_F32 || in->is_float) {
    materialize(fn, &in->lhs, MC_F32, x, size);
    return MC_F32;
  }
  {
    MslClass as = class_is_64(source_cls)
                      ? (in->is_unsigned ? MC_U64 : MC_I64)
                      : (in->is_unsigned ? MC_U32 : MC_I32);
    if (source_cls == MC_U32 && !in->is_unsigned) {
      as = MC_U32;
    }
    if (source_cls == MC_U64 && !in->is_unsigned) {
      as = MC_U64;
    }
    materialize(fn, &in->lhs, as, x, size);
    return as;
  }
}

static void emit_cast_store(MslFn *fn, const IRInstruction *in,
                            MtlcTypeKind target, MslClass source_cls,
                            const char *x) {
  char expr[2600];
  switch (target) {
  case MTLC_TYPE_FLOAT32:
    snprintf(expr, sizeof(expr), "(float)(%s)", x);
    store_dest(fn, in, expr, MC_F32);
    return;
  case MTLC_TYPE_FLOAT16:
    snprintf(expr, sizeof(expr), "(float)(half)(%s)", x);
    store_dest(fn, in, expr, MC_F32);
    return;
  case MTLC_TYPE_BFLOAT16:
    if (!version_at_least(fn->m, 3, 1)) {
      mod_error(fn->m, "Metal: bfloat16 needs --metal-version=3.1 or later");
      return;
    }
    snprintf(expr, sizeof(expr), "(float)(bfloat)(%s)", x);
    store_dest(fn, in, expr, MC_F32);
    return;
  case MTLC_TYPE_BOOL:
    if (source_cls == MC_F32) {
      snprintf(expr, sizeof(expr), "(uint)(%s != 0.0f)", x);
    } else {
      snprintf(expr, sizeof(expr), "(uint)(%s != 0)", x);
    }
    store_dest(fn, in, expr, MC_U32);
    return;
  case MTLC_TYPE_INT8:
  case MTLC_TYPE_INT16:
    if (source_cls == MC_F32) {
      snprintf(expr, sizeof(expr), "(int)(%s)(int)(%s)",
               storage_type(target), x);
    } else {
      snprintf(expr, sizeof(expr), "(int)(%s)(%s)", storage_type(target), x);
    }
    store_dest(fn, in, expr, MC_I32);
    return;
  case MTLC_TYPE_UINT8:
  case MTLC_TYPE_UINT16:
    if (source_cls == MC_F32) {
      snprintf(expr, sizeof(expr), "(uint)(%s)(uint)(%s)",
               storage_type(target), x);
    } else {
      snprintf(expr, sizeof(expr), "(uint)(%s)(%s)", storage_type(target), x);
    }
    store_dest(fn, in, expr, MC_U32);
    return;
  case MTLC_TYPE_INT32:
  case MTLC_TYPE_ENUM:
    if (source_cls == MC_F32) {
      snprintf(expr, sizeof(expr), "(int)(%s)", x);
      store_dest(fn, in, expr, MC_I32);
    } else {
      convert_expr(fn, expr, sizeof(expr), x, source_cls, MC_I32);
      store_dest(fn, in, expr, MC_I32);
    }
    return;
  case MTLC_TYPE_UINT32:
    if (source_cls == MC_F32) {
      snprintf(expr, sizeof(expr), "(uint)(%s)", x);
    } else {
      snprintf(expr, sizeof(expr), "(uint)(%s)", x);
    }
    store_dest(fn, in, expr, MC_U32);
    return;
  case MTLC_TYPE_INT64:
    snprintf(expr, sizeof(expr), "(long)(%s)", x);
    store_dest(fn, in, expr, MC_I64);
    return;
  case MTLC_TYPE_UINT64:
    snprintf(expr, sizeof(expr), "(ulong)(%s)", x);
    store_dest(fn, in, expr, MC_U64);
    return;
  default:
    mod_error(fn->m, "Metal: '%s' casts to an unsupported type '%s'",
              fn_name(fn), in->text ? in->text : "?");
    return;
  }
}

static void emit_cast(MslFn *fn, const IRInstruction *in) {
  MtlcTypeKind target = cast_kind(in);
  MslClass source_cls = operand_class(fn, &in->lhs);
  MslVar *dest = dest_var(fn, in);
  char x[2048];
  if (!dest) {
    return;
  }
  if (target == MTLC_TYPE_FLOAT64 && dest->demoted) {
    target = MTLC_TYPE_FLOAT32;
  }
  if (target == MTLC_TYPE_FLOAT64 || source_cls == MC_F64) {
    float64_refusal(fn, in);
    return;
  }
  if (class_of_kind(target) == MC_PTR) {
    emit_cast_to_pointer(fn, in, dest);
    return;
  }
  if (class_of_kind(target) == MC_REC) {
    materialize(fn, &in->lhs, MC_REC, x, sizeof(x));
    line(fn, "%s = %s;", dest->ident, x);
    return;
  }
  if (!check_f64(fn, class_of_kind(target))) {
    return;
  }
  if (source_cls == MC_F64 && !check_f64(fn, MC_F64)) {
    return;
  }
  source_cls = cast_source(fn, in, source_cls, x, sizeof(x));
  emit_cast_store(fn, in, target, source_cls, x);
}

static void emit_select(MslFn *fn, const IRInstruction *in) {
  MslVar *dest = dest_var(fn, in);
  char cond[4200], a[2048], b[2048], expr[8600];
  MslClass want;
  if (!dest) {
    return;
  }
  want = dest->cls;
  if (in->text && in->argument_count > 1) {
    MslClass l = operand_class(fn, &in->lhs);
    MslClass r = operand_class(fn, &in->arguments[1]);
    char x[2048], y[2048];
    if (l == MC_F32 || r == MC_F32 || in->is_float) {
      materialize(fn, &in->lhs, MC_F32, x, sizeof(x));
      materialize(fn, &in->arguments[1], MC_F32, y, sizeof(y));
    } else {
      int wide = class_is_64(l) || class_is_64(r);
      MslClass c = in->is_unsigned ? (wide ? MC_U64 : MC_U32)
                                   : (wide ? MC_I64 : MC_I32);
      if (!strcmp(in->text, "==") || !strcmp(in->text, "!=")) {
        c = wide ? MC_U64 : MC_U32;
      }
      materialize(fn, &in->lhs, c, x, sizeof(x));
      materialize(fn, &in->arguments[1], c, y, sizeof(y));
    }
    snprintf(cond, sizeof(cond), "(%s %s %s)", x, in->text, y);
  } else {
    MslClass l = operand_class(fn, &in->lhs);
    char x[2048];
    materialize(fn, &in->lhs, l == MC_F32 ? MC_F32 : (class_is_64(l) ? MC_U64 : MC_U32),
                x, sizeof(x));
    snprintf(cond, sizeof(cond), "(%s != 0)", x);
  }
  materialize(fn, &in->rhs, want, a, sizeof(a));
  if (in->argument_count > 0) {
    materialize(fn, &in->arguments[0], want, b, sizeof(b));
  } else {
    snprintf(b, sizeof(b), "0");
  }
  snprintf(expr, sizeof(expr), "%s ? %s : %s", cond, a, b);
  if (dest->cls == MC_PTR || dest->cls == MC_REC) {
    line(fn, "%s = %s;", dest->ident, expr);
  } else {
    store_dest(fn, in, expr, want);
  }
}

static void emit_address_of(MslFn *fn, const IRInstruction *in) {
  MslVar *home = operand_var(fn, &in->lhs);
  MslVar *dest = dest_var(fn, in);
  if (!dest) {
    return;
  }
  if (!home) {
    mod_error(fn->m, "Metal: '%s' takes the address of '%s', which has no "
                     "storage in device code",
              fn_name(fn), source_name(in->lhs.name));
    return;
  }
  if (home->alloc) {
    line(fn, "%s = %s;", dest->ident, home->ident);
    return;
  }
  line(fn, "%s = (thread uchar*)&%s;", dest->ident, home->ident);
}

static const char *order_name(MtlcMemoryOrder order) {
  switch (order) {
  case MTLC_MEMORY_ORDER_ACQUIRE: return "memory_order_acquire";
  case MTLC_MEMORY_ORDER_RELEASE: return "memory_order_release";
  case MTLC_MEMORY_ORDER_ACQ_REL: return "memory_order_acq_rel";
  case MTLC_MEMORY_ORDER_SEQ_CST:
  case MTLC_MEMORY_ORDER_DEFAULT:
    return "memory_order_seq_cst";
  default:
    return "memory_order_relaxed";
  }
}

static const char *scope_name(MtlcMemoryScope scope, int workgroup_space) {
  switch (scope) {
  case MTLC_MEMORY_SCOPE_SUBGROUP: return "thread_scope_simdgroup";
  case MTLC_MEMORY_SCOPE_WORKGROUP: return "thread_scope_threadgroup";
  case MTLC_MEMORY_SCOPE_WORK_ITEM: return "thread_scope_thread";
  case MTLC_MEMORY_SCOPE_DEFAULT:
    return workgroup_space ? "thread_scope_threadgroup" : "thread_scope_device";
  default:
    return "thread_scope_device";
  }
}

static int order_acquires(MtlcMemoryOrder order) {
  return order == MTLC_MEMORY_ORDER_ACQUIRE ||
         order == MTLC_MEMORY_ORDER_ACQ_REL ||
         order == MTLC_MEMORY_ORDER_SEQ_CST ||
         order == MTLC_MEMORY_ORDER_DEFAULT;
}

static int order_releases(MtlcMemoryOrder order) {
  return order == MTLC_MEMORY_ORDER_RELEASE ||
         order == MTLC_MEMORY_ORDER_ACQ_REL ||
         order == MTLC_MEMORY_ORDER_SEQ_CST ||
         order == MTLC_MEMORY_ORDER_DEFAULT;
}

static void emit_atomic(MslFn *fn, const IRInstruction *in) {
  MtlcIntrinsic intrinsic = in->intrinsic;
  int is64 = ir_intrinsic_atomic_value_kind(intrinsic) == MTLC_TYPE_UINT64;
  int is_cas = ir_intrinsic_is_compare_exchange(intrinsic);
  int is_load = ir_intrinsic_is_atomic_load(intrinsic);
  int is_store = ir_intrinsic_is_atomic_store(intrinsic);
  MslSpace space;
  MslSpace declared = in->address_space == MTLC_ADDRESS_SPACE_WORKGROUP
                          ? MS_TG
                          : MS_DEVICE;
  char base[2048], index[2048], value[2048], desired[2048], ptr[4400];
  char result[5200];
  const char *order = "memory_order_relaxed";
  int fence_before = 0, fence_after = 0;
  const char *flags = declared == MS_TG ? "mem_flags::mem_threadgroup"
                                        : "mem_flags::mem_device";
  const char *scope =
      scope_name(in->memory_scope, declared == MS_TG);
  MtlcMemoryOrder ordering = in->memory_order;
  if (in->argument_count < (size_t)ir_intrinsic_arity(intrinsic)) {
    mod_error(fn->m, "Metal: malformed atomic in '%s'", fn_name(fn));
    return;
  }
  if (is64) {
    mod_error(fn->m,
              "Metal has no 64-bit atomic read-modify-write that returns a "
              "value: '%s' calls %s; Apple GPUs offer only void "
              "atomic_max/atomic_min on atomic_ulong. Use a uint32 atomic",
              fn_name(fn), ir_intrinsic_name(intrinsic));
    return;
  }
  if (in->address_space == MTLC_ADDRESS_SPACE_CONSTANT ||
      in->address_space == MTLC_ADDRESS_SPACE_PRIVATE) {
    mod_error(fn->m, "Metal: atomic on constant or private memory in '%s'",
              fn_name(fn));
    return;
  }
  pointer_expr(fn, &in->arguments[0], &space, base, sizeof(base));
  if (space != declared) {
    mod_error(fn->m,
              "Metal: the atomic in '%s' names %s memory, and its pointer "
              "holds %s address",
              fn_name(fn), declared == MS_TG ? "workgroup" : "global",
              space_name(space));
    return;
  }
  materialize(fn, &in->arguments[1], MC_I64, index, sizeof(index));
  snprintf(ptr, sizeof(ptr), "(%s atomic_uint*)(%s + %s * 4l)",
           space_qual(fn->m, space), base, index);
  if (ordering != MTLC_MEMORY_ORDER_RELAXED) {
    if (version_at_least(fn->m, 4, 1)) {
      order = order_name(ordering);
      if (is_load && !strcmp(order, "memory_order_release")) {
        order = "memory_order_relaxed";
      }
    } else if (version_at_least(fn->m, 3, 2)) {
      fence_before = order_releases(ordering) && !is_load;
      fence_after = order_acquires(ordering) && !is_store;
      if (ordering == MTLC_MEMORY_ORDER_SEQ_CST ||
          ordering == MTLC_MEMORY_ORDER_DEFAULT) {
        fence_before = fence_after = 1;
      }
    } else {
      mod_error(fn->m,
                "Metal %d.%d atomics are relaxed only: '%s' asks for an "
                "ordered %s; build with --metal-version=3.2 or later",
                fn->m->options.version_major, fn->m->options.version_minor,
                fn_name(fn), ir_intrinsic_name(intrinsic));
      return;
    }
  }
  if (fence_before || fence_after) {
    report_note(fn, in, "atomic", "ordered atomic", IR_DEVICE_FACT,
                fence_before && fence_after
                    ? "a relaxed atomic between two atomic_thread_fence calls"
                    : fence_before
                          ? "atomic_thread_fence before a relaxed atomic"
                          : "a relaxed atomic before atomic_thread_fence",
                "Metal 3.2 atomics take no memory order; 4.1 has native ones",
                "atomic-fence");
  }
  if (fence_before) {
    line(fn, "atomic_thread_fence(%s, memory_order_seq_cst, %s);", flags,
         scope);
  }
  if (is_load) {
    snprintf(result, sizeof(result), "atomic_load_explicit(%s, %s)", ptr,
             order);
    store_dest(fn, in, result, MC_U32);
  } else if (is_store) {
    materialize(fn, &in->arguments[2], MC_U32, value, sizeof(value));
    line(fn, "atomic_store_explicit(%s, %s, %s);", ptr, value, order);
  } else if (is_cas) {
    const char *failure = "memory_order_relaxed";
    size_t id = fn->temp_counter++;
    materialize(fn, &in->arguments[2], MC_U32, value, sizeof(value));
    materialize(fn, &in->arguments[3], MC_U32, desired, sizeof(desired));
    if (version_at_least(fn->m, 4, 1) &&
        in->failure_memory_order != MTLC_MEMORY_ORDER_RELAXED) {
      failure = order_name(in->failure_memory_order);
    }
    line(fn, "{");
    fn->indent++;
    line(fn, "uint mtl_want_%zu = %s;", id, value);
    line(fn, "uint mtl_seen_%zu = mtl_want_%zu;", id, id);
    line(fn,
         "while (!atomic_compare_exchange_weak_explicit(%s, &mtl_seen_%zu, "
         "%s, %s, %s)) {",
         ptr, id, desired, order, failure);
    fn->indent++;
    line(fn, "if (mtl_seen_%zu != mtl_want_%zu) {", id, id);
    fn->indent++;
    line(fn, "break;");
    fn->indent--;
    line(fn, "}");
    fn->indent--;
    line(fn, "}");
    snprintf(result, sizeof(result), "mtl_seen_%zu", id);
    store_dest(fn, in, result, MC_U32);
    fn->indent--;
    line(fn, "}");
  } else {
    const char *op = NULL;
    switch (intrinsic) {
    case MTLC_INTRINSIC_GPU_ATOMIC_ADD_U32: op = "atomic_fetch_add_explicit"; break;
    case MTLC_INTRINSIC_GPU_ATOMIC_SUB_U32: op = "atomic_fetch_sub_explicit"; break;
    case MTLC_INTRINSIC_GPU_ATOMIC_MIN_U32: op = "atomic_fetch_min_explicit"; break;
    case MTLC_INTRINSIC_GPU_ATOMIC_MAX_U32: op = "atomic_fetch_max_explicit"; break;
    case MTLC_INTRINSIC_GPU_ATOMIC_AND_U32: op = "atomic_fetch_and_explicit"; break;
    case MTLC_INTRINSIC_GPU_ATOMIC_OR_U32: op = "atomic_fetch_or_explicit"; break;
    case MTLC_INTRINSIC_GPU_ATOMIC_XOR_U32: op = "atomic_fetch_xor_explicit"; break;
    case MTLC_INTRINSIC_GPU_ATOMIC_EXCHANGE_U32: op = "atomic_exchange_explicit"; break;
    default: break;
    }
    if (!op) {
      mod_error(fn->m, "Metal: unsupported atomic %s in '%s'",
                ir_intrinsic_name(intrinsic), fn_name(fn));
      return;
    }
    materialize(fn, &in->arguments[2], MC_U32, value, sizeof(value));
    snprintf(result, sizeof(result), "%s(%s, %s, %s)", op, ptr, value, order);
    if (dest_var(fn, in)) {
      store_dest(fn, in, result, MC_U32);
    } else {
      line(fn, "%s;", result);
    }
  }
  if (fence_after) {
    line(fn, "atomic_thread_fence(%s, memory_order_seq_cst, %s);", flags,
         scope);
  }
}

static void emit_subgroup(MslFn *fn, const IRInstruction *in) {
  MtlcIntrinsic intrinsic = in->intrinsic;
  char x[2048], y[2048], expr[4800];
  int is_float = ir_intrinsic_subgroup_result_kind(intrinsic) ==
                 MTLC_TYPE_FLOAT32;
  MslClass value_cls = is_float ? MC_F32 : MC_U32;
  switch (intrinsic) {
  case MTLC_INTRINSIC_GPU_SUBGROUP_LOCAL_ID:
    store_dest(fn, in, "mtl_lane", MC_U32);
    return;
  case MTLC_INTRINSIC_GPU_SUBGROUP_SIZE:
    store_dest(fn, in, "mtl_lanes", MC_U32);
    return;
  case MTLC_INTRINSIC_GPU_SUBGROUP_BROADCAST_U32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_BROADCAST_F32:
    materialize(fn, &in->arguments[0], value_cls, x, sizeof(x));
    materialize(fn, &in->arguments[1], MC_U32, y, sizeof(y));
    snprintf(expr, sizeof(expr), "simd_broadcast(%s, (ushort)(%s))", x, y);
    store_dest(fn, in, expr, value_cls);
    return;
  case MTLC_INTRINSIC_GPU_SUBGROUP_SHUFFLE_U32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_SHUFFLE_F32: {
    size_t id = fn->temp_counter++;
    materialize(fn, &in->arguments[0], value_cls, x, sizeof(x));
    materialize(fn, &in->arguments[1], MC_U32, y, sizeof(y));
    line(fn, "{");
    fn->indent++;
    line(fn, "%s mtl_v_%zu = %s;", class_type(value_cls), id, x);
    line(fn, "uint mtl_src_%zu = %s;", id, y);
    line(fn,
         "ulong mtl_mask_%zu = (ulong)(simd_vote::vote_t)"
         "simd_active_threads_mask();",
         id);
    line(fn,
         "bool mtl_ok_%zu = mtl_src_%zu < mtl_lanes && ((mtl_mask_%zu >> "
         "mtl_src_%zu) & 1ul) != 0ul;",
         id, id, id, id);
    line(fn,
         "%s mtl_got_%zu = simd_shuffle(mtl_v_%zu, (ushort)(mtl_ok_%zu ? "
         "mtl_src_%zu : mtl_lane));",
         class_type(value_cls), id, id, id, id);
    snprintf(expr, sizeof(expr), "mtl_ok_%zu ? mtl_got_%zu : mtl_v_%zu", id,
             id, id);
    store_dest(fn, in, expr, value_cls);
    fn->indent--;
    line(fn, "}");
    return;
  }
  case MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_ADD_U32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_ADD_F32:
    materialize(fn, &in->arguments[0], value_cls, x, sizeof(x));
    snprintf(expr, sizeof(expr), "simd_sum(%s)", x);
    store_dest(fn, in, expr, value_cls);
    return;
  case MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MIN_U32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MIN_F32:
    materialize(fn, &in->arguments[0], value_cls, x, sizeof(x));
    snprintf(expr, sizeof(expr), "simd_min(%s)", x);
    store_dest(fn, in, expr, value_cls);
    return;
  case MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MAX_U32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MAX_F32:
    materialize(fn, &in->arguments[0], value_cls, x, sizeof(x));
    snprintf(expr, sizeof(expr), "simd_max(%s)", x);
    store_dest(fn, in, expr, value_cls);
    return;
  case MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_INCLUSIVE_ADD_U32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_INCLUSIVE_ADD_F32:
    materialize(fn, &in->arguments[0], value_cls, x, sizeof(x));
    snprintf(expr, sizeof(expr), "simd_prefix_inclusive_sum(%s)", x);
    store_dest(fn, in, expr, value_cls);
    return;
  case MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_EXCLUSIVE_ADD_U32:
  case MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_EXCLUSIVE_ADD_F32:
    materialize(fn, &in->arguments[0], value_cls, x, sizeof(x));
    snprintf(expr, sizeof(expr), "simd_prefix_exclusive_sum(%s)", x);
    store_dest(fn, in, expr, value_cls);
    return;
  case MTLC_INTRINSIC_GPU_SUBGROUP_BALLOT_WORD: {
    size_t id = fn->temp_counter++;
    materialize(fn, &in->arguments[0], MC_U32, x, sizeof(x));
    materialize(fn, &in->arguments[1], MC_U32, y, sizeof(y));
    line(fn, "{");
    fn->indent++;
    line(fn,
         "ulong mtl_vote_%zu = (ulong)(simd_vote::vote_t)simd_ballot(%s != "
         "0u);",
         id, x);
    line(fn, "uint mtl_word_%zu = %s;", id, y);
    line(fn,
         "ulong mtl_valid_%zu = mtl_lanes >= 64u ? ~0ul : ((1ul << mtl_lanes) "
         "- 1ul);",
         id);
    snprintf(expr, sizeof(expr),
             "mtl_word_%zu < 2u ? (uint)((mtl_vote_%zu & mtl_valid_%zu) >> "
             "(32u * mtl_word_%zu)) : 0u",
             id, id, id, id);
    store_dest(fn, in, expr, MC_U32);
    fn->indent--;
    line(fn, "}");
    return;
  }
  case MTLC_INTRINSIC_GPU_SUBGROUP_ANY:
    materialize(fn, &in->arguments[0], MC_U32, x, sizeof(x));
    snprintf(expr, sizeof(expr), "(uint)simd_any(%s != 0u)", x);
    store_dest(fn, in, expr, MC_U32);
    return;
  case MTLC_INTRINSIC_GPU_SUBGROUP_ALL:
    materialize(fn, &in->arguments[0], MC_U32, x, sizeof(x));
    snprintf(expr, sizeof(expr), "(uint)simd_all(%s != 0u)", x);
    store_dest(fn, in, expr, MC_U32);
    return;
  default:
    mod_error(fn->m, "Metal: unsupported subgroup operation %s in '%s'",
              ir_intrinsic_name(intrinsic), fn_name(fn));
    return;
  }
}

static void emit_math(MslFn *fn, const IRInstruction *in) {
  const char *prefix = fn->m->options.fast_math ? "fast::" : "precise::";
  const char *name;
  char x[2048], expr[2200];
  switch (in->intrinsic) {
  case MTLC_INTRINSIC_GPU_SQRT_F32: name = "sqrt"; break;
  case MTLC_INTRINSIC_GPU_RSQRT_F32: name = "rsqrt"; break;
  case MTLC_INTRINSIC_GPU_ABS_F32: name = "fabs"; prefix = ""; break;
  case MTLC_INTRINSIC_GPU_SIN_F32: name = "sin"; break;
  case MTLC_INTRINSIC_GPU_COS_F32: name = "cos"; break;
  case MTLC_INTRINSIC_GPU_LOG_F32: name = "log"; break;
  default: name = "exp"; break;
  }
  materialize(fn, &in->arguments[0], MC_F32, x, sizeof(x));
  snprintf(expr, sizeof(expr), "%s%s(%s)", prefix, name, x);
  store_dest(fn, in, expr, MC_F32);
}

static int emit_bits(MslFn *fn, const IRInstruction *in) {
  char a[2048], b[2048], c[2048], expr[6400];
  switch (in->intrinsic) {
  case MTLC_INTRINSIC_GPU_F16_BITS_TO_F32:
  case MTLC_INTRINSIC_GPU_H2F_LO:
    materialize(fn, &in->arguments[0], MC_U32, a, sizeof(a));
    snprintf(expr, sizeof(expr), "(float)as_type<half>((ushort)(%s))", a);
    store_dest(fn, in, expr, MC_F32);
    return 1;
  case MTLC_INTRINSIC_GPU_H2F_HI:
    materialize(fn, &in->arguments[0], MC_U32, a, sizeof(a));
    snprintf(expr, sizeof(expr), "(float)as_type<half>((ushort)(%s >> 16))",
             a);
    store_dest(fn, in, expr, MC_F32);
    return 1;
  case MTLC_INTRINSIC_GPU_F32_TO_F16_BITS:
    materialize(fn, &in->arguments[0], MC_F32, a, sizeof(a));
    snprintf(expr, sizeof(expr), "(uint)as_type<ushort>((half)(%s))", a);
    store_dest(fn, in, expr, MC_U32);
    return 1;
  case MTLC_INTRINSIC_GPU_F2H2:
    materialize(fn, &in->arguments[0], MC_F32, a, sizeof(a));
    materialize(fn, &in->arguments[1], MC_F32, b, sizeof(b));
    snprintf(expr, sizeof(expr), "as_type<uint>(half2((half)(%s), (half)(%s)))",
             a, b);
    store_dest(fn, in, expr, MC_U32);
    return 1;
  case MTLC_INTRINSIC_GPU_F32_FROM_BITS:
    materialize(fn, &in->arguments[0], MC_U32, a, sizeof(a));
    snprintf(expr, sizeof(expr), "as_type<float>(%s)", a);
    store_dest(fn, in, expr, MC_F32);
    return 1;
  case MTLC_INTRINSIC_GPU_F32_TO_BITS:
    materialize(fn, &in->arguments[0], MC_F32, a, sizeof(a));
    snprintf(expr, sizeof(expr), "as_type<uint>(%s)", a);
    store_dest(fn, in, expr, MC_U32);
    return 1;
  case MTLC_INTRINSIC_GPU_BF2F:
    materialize(fn, &in->arguments[0], MC_U32, a, sizeof(a));
    snprintf(expr, sizeof(expr), "as_type<float>(%s << 16)", a);
    store_dest(fn, in, expr, MC_F32);
    return 1;
  case MTLC_INTRINSIC_GPU_WORKGROUP_UNIFORM_U32:
    materialize(fn, &in->arguments[0], MC_U32, a, sizeof(a));
    store_dest(fn, in, a, MC_U32);
    return 1;
  case MTLC_INTRINSIC_GPU_F2BF:
    materialize(fn, &in->arguments[0], MC_F32, a, sizeof(a));
    snprintf(expr, sizeof(expr),
             "(as_type<uint>(%s) + 32767u + ((as_type<uint>(%s) >> 16) & 1u)) "
             ">> 16",
             a, a);
    store_dest(fn, in, expr, MC_U32);
    return 1;
  case MTLC_INTRINSIC_GPU_HADD2:
  case MTLC_INTRINSIC_GPU_HMUL2:
    materialize(fn, &in->arguments[0], MC_U32, a, sizeof(a));
    materialize(fn, &in->arguments[1], MC_U32, b, sizeof(b));
    snprintf(expr, sizeof(expr), "as_type<uint>(as_type<half2>(%s) %s as_type<half2>(%s))",
             a, in->intrinsic == MTLC_INTRINSIC_GPU_HADD2 ? "+" : "*", b);
    store_dest(fn, in, expr, MC_U32);
    return 1;
  case MTLC_INTRINSIC_GPU_HFMA2:
    materialize(fn, &in->arguments[0], MC_U32, a, sizeof(a));
    materialize(fn, &in->arguments[1], MC_U32, b, sizeof(b));
    materialize(fn, &in->arguments[2], MC_U32, c, sizeof(c));
    snprintf(expr, sizeof(expr),
             "as_type<uint>(fma(as_type<half2>(%s), as_type<half2>(%s), "
             "as_type<half2>(%s)))",
             a, b, c);
    store_dest(fn, in, expr, MC_U32);
    return 1;
  case MTLC_INTRINSIC_GPU_DP4A_U32:
  case MTLC_INTRINSIC_GPU_DP4A_S32:
  case MTLC_INTRINSIC_GPU_DP2A_LO_U32:
  case MTLC_INTRINSIC_GPU_DP2A_HI_U32:
  case MTLC_INTRINSIC_GPU_DP2A_LO_S32:
  case MTLC_INTRINSIC_GPU_DP2A_HI_S32: {
    int is_signed = in->intrinsic == MTLC_INTRINSIC_GPU_DP4A_S32 ||
                    in->intrinsic == MTLC_INTRINSIC_GPU_DP2A_LO_S32 ||
                    in->intrinsic == MTLC_INTRINSIC_GPU_DP2A_HI_S32;
    MslClass cls = is_signed ? MC_I32 : MC_U32;
    materialize(fn, &in->arguments[0], cls, a, sizeof(a));
    materialize(fn, &in->arguments[1], cls, b, sizeof(b));
    materialize(fn, &in->arguments[2], cls, c, sizeof(c));
    if (in->intrinsic == MTLC_INTRINSIC_GPU_DP4A_U32) {
      fn->m->needs |= NEED_DP4A_U;
      snprintf(expr, sizeof(expr), "mtl_dp4a_u32(%s, %s, %s)", a, b, c);
    } else if (in->intrinsic == MTLC_INTRINSIC_GPU_DP4A_S32) {
      fn->m->needs |= NEED_DP4A_S;
      snprintf(expr, sizeof(expr), "mtl_dp4a_s32(%s, %s, %s)", a, b, c);
    } else if (!is_signed) {
      fn->m->needs |= NEED_DP2A_U;
      snprintf(expr, sizeof(expr), "mtl_dp2a_u32(%s, %s, %s, %su)", a, b, c,
               in->intrinsic == MTLC_INTRINSIC_GPU_DP2A_HI_U32 ? "1" : "0");
    } else {
      fn->m->needs |= NEED_DP2A_S;
      snprintf(expr, sizeof(expr), "mtl_dp2a_s32(%s, %s, %s, %su)", a, b, c,
               in->intrinsic == MTLC_INTRINSIC_GPU_DP2A_HI_S32 ? "1" : "0");
    }
    store_dest(fn, in, expr, cls);
    return 1;
  }
  case MTLC_INTRINSIC_GPU_PRMT_B32:
    fn->m->needs |= NEED_PRMT;
    materialize(fn, &in->arguments[0], MC_U32, a, sizeof(a));
    materialize(fn, &in->arguments[1], MC_U32, b, sizeof(b));
    materialize(fn, &in->arguments[2], MC_U32, c, sizeof(c));
    snprintf(expr, sizeof(expr), "mtl_prmt(%s, %s, %s)", a, b, c);
    store_dest(fn, in, expr, MC_U32);
    return 1;
  case MTLC_INTRINSIC_GPU_LOAD4_F32:
  case MTLC_INTRINSIC_GPU_LOAD4_U32:
  case MTLC_INTRINSIC_GPU_STORE4_F32:
  case MTLC_INTRINSIC_GPU_STORE4_U32: {
    int load = in->intrinsic == MTLC_INTRINSIC_GPU_LOAD4_F32 ||
               in->intrinsic == MTLC_INTRINSIC_GPU_LOAD4_U32;
    int is_float = in->intrinsic == MTLC_INTRINSIC_GPU_LOAD4_F32 ||
                   in->intrinsic == MTLC_INTRINSIC_GPU_STORE4_F32;
    const char *vec = is_float ? "float4" : "uint4";
    const char *packed = is_float ? "packed_float4" : "packed_uint4";
    MslSpace vs, ss;
    pointer_expr(fn, &in->arguments[0], &vs, a, sizeof(a));
    pointer_expr(fn, &in->arguments[1], &ss, b, sizeof(b));
    if (load) {
      line(fn, "*(%s %s*)(%s) = %s(*(%s %s*)(%s));", space_qual(fn->m, ss),
           packed, b, packed, space_qual(fn->m, vs), vec, a);
    } else {
      line(fn, "*(%s %s*)(%s) = %s(*(%s %s*)(%s));", space_qual(fn->m, vs),
           vec, a, vec, space_qual(fn->m, ss), packed, b);
    }
    return 1;
  }
  default:
    return 0;
  }
}

static const char *format_literal(MslFn *fn, const IROperand *op) {
  MslVar *var;
  if (op->kind == IR_OPERAND_STRING) {
    return op->name;
  }
  var = operand_var(fn, op);
  return var ? var->literal : NULL;
}

static void quote_string(const char *text, Sb *out) {
  sb_puts(out, "\"");
  for (const unsigned char *p = (const unsigned char *)text; p && *p; p++) {
    char buffer[8];
    if (*p == '"' || *p == '\\') {
      buffer[0] = '\\';
      buffer[1] = (char)*p;
      buffer[2] = '\0';
    } else if (*p == '\n') {
      snprintf(buffer, sizeof(buffer), "\\n");
    } else if (*p == '\t') {
      snprintf(buffer, sizeof(buffer), "\\t");
    } else if (*p < 0x20 || *p >= 0x7f) {
      snprintf(buffer, sizeof(buffer), "\\%03o", (unsigned)*p);
    } else {
      buffer[0] = (char)*p;
      buffer[1] = '\0';
    }
    sb_puts(out, buffer);
  }
  sb_puts(out, "\"");
}

static void emit_print(MslFn *fn, const IRInstruction *in) {
  const char *format;
  Sb text = {0};
  char a[2048], b[2048];
  if (in->argument_count < 1) {
    mod_error(fn->m, "Metal: malformed print in '%s'", fn_name(fn));
    return;
  }
  if (!version_at_least(fn->m, 3, 2)) {
    mod_error(fn->m,
              "Metal: '%s' prints from the device, which needs Metal "
              "logging: --metal-version=3.2 or later",
              fn_name(fn));
    return;
  }
  format = format_literal(fn, &in->arguments[0]);
  if (!format) {
    mod_error(fn->m, "Metal: a kernel print in '%s' needs a literal format "
                     "string",
              fn_name(fn));
    return;
  }
  quote_string(format, &text);
  switch (in->intrinsic) {
  case MTLC_INTRINSIC_GPU_PRINT_I32:
    materialize(fn, &in->arguments[1], MC_I32, a, sizeof(a));
    line(fn, "os_log_default.log(%s, %s);", sb_str(&text), a);
    break;
  case MTLC_INTRINSIC_GPU_PRINT_F32:
    materialize(fn, &in->arguments[1], MC_F32, a, sizeof(a));
    line(fn, "os_log_default.log(%s, %s);", sb_str(&text), a);
    break;
  case MTLC_INTRINSIC_GPU_PRINT_2I32:
    materialize(fn, &in->arguments[1], MC_I32, a, sizeof(a));
    materialize(fn, &in->arguments[2], MC_I32, b, sizeof(b));
    line(fn, "os_log_default.log(%s, %s, %s);", sb_str(&text), a, b);
    break;
  default:
    line(fn, "os_log_default.log(%s);", sb_str(&text));
    break;
  }
  sb_free(&text);
}

static void emit_assert(MslFn *fn, const IRInstruction *in) {
  char condition[2048];
  if (!fn->m->options.gpu_checks || in->argument_count < 1) {
    return;
  }
  if (!version_at_least(fn->m, 3, 2)) {
    mod_error(fn->m,
              "Metal: --gpu-checks reports a failed gpu_assert through Metal "
              "logging, which needs --metal-version=3.2 or later");
    return;
  }
  materialize(fn, &in->arguments[0], MC_U32, condition, sizeof(condition));
  line(fn, "if (%s == 0u) {", condition);
  fn->indent++;
  line(fn,
       "os_log_default.log_fault(\"mettle: gpu_assert failed in %s, line "
       "%zu\");",
       fn_name(fn), in->location.line);
  fn->indent--;
  line(fn, "}");
}

static void emit_call_args(MslFn *fn, const IRInstruction *in,
                           MslSpec *callee_spec, Sb *args) {
  MslFn *callee_fn = callee_spec->fn;
  IRFunction *callee = callee_fn->func;
  for (size_t a = 0; a < in->argument_count && a < callee->parameter_count;
       a++) {
    char value[2048];
    MslVar *param = callee->parameter_names && callee->parameter_names[a]
                        ? find_var(callee_fn, callee->parameter_names[a])
                        : NULL;
    MslClass want = param ? param->cls : MC_I32;
    if (a) {
      sb_puts(args, ", ");
    }
    if (want == MC_REC) {
      MslVar *source = operand_var(fn, &in->arguments[a]);
      sb_puts(args, source ? source->ident : "0");
      continue;
    }
    if (want != MC_PTR && param && param->storage != MTLC_TYPE_VOID &&
        class_of_kind(param->storage) != MC_NONE) {
      want = class_of_kind(param->storage);
    }
    materialize(fn, &in->arguments[a], want, value, sizeof(value));
    sb_puts(args, value);
  }
  {
    unsigned builtins = callee_spec->builtins;
    static const struct {
      unsigned bit;
      const char *name;
    } order[] = {{MB_TID, "mtl_tid"},     {MB_CTAID, "mtl_ctaid"},
                 {MB_NTID, "mtl_ntid"},   {MB_NCTAID, "mtl_nctaid"},
                 {MB_LANE, "mtl_lane"},   {MB_LANES, "mtl_lanes"}};
    int first = in->argument_count == 0;
    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
      if (builtins & order[i].bit) {
        if (!first) {
          sb_puts(args, ", ");
        }
        sb_puts(args, order[i].name);
        first = 0;
      }
    }
  }
}

static void emit_call(MslFn *fn, const IRInstruction *in, size_t index) {
  MtlcIntrinsic intrinsic = in->intrinsic;
  unsigned bit;
  const char *base;
  int component = builtin_component(intrinsic, &bit, &base);
  if (component >= 0) {
    char expr[64];
    snprintf(expr, sizeof(expr), "%s.%c", base, "xyz"[component]);
    store_dest(fn, in, expr, MC_U32);
    return;
  }
  if (intrinsic == MTLC_INTRINSIC_GPU_WORKGROUP_BARRIER) {
    line(fn, "threadgroup_barrier(mem_flags::mem_threadgroup);");
    return;
  }
  if (ir_intrinsic_is_subgroup(intrinsic)) {
    emit_subgroup(fn, in);
    return;
  }
  if (ir_intrinsic_is_atomic(intrinsic)) {
    emit_atomic(fn, in);
    return;
  }
  if (is_math_intrinsic(intrinsic)) {
    emit_math(fn, in);
    return;
  }
  if (intrinsic != MTLC_INTRINSIC_NONE) {
    if (emit_bits(fn, in)) {
      return;
    }
    switch (intrinsic) {
    case MTLC_INTRINSIC_GPU_PRINT:
    case MTLC_INTRINSIC_GPU_PRINT_I32:
    case MTLC_INTRINSIC_GPU_PRINT_F32:
    case MTLC_INTRINSIC_GPU_PRINT_2I32:
      emit_print(fn, in);
      return;
    case MTLC_INTRINSIC_GPU_ASSERT:
      emit_assert(fn, in);
      return;
    case MTLC_INTRINSIC_GPU_MBARRIER_INIT:
    case MTLC_INTRINSIC_GPU_MBARRIER_ARRIVE_EXPECT_TX:
    case MTLC_INTRINSIC_GPU_MBARRIER_WAIT_PARITY:
    case MTLC_INTRINSIC_GPU_FENCE_MBARRIER_INIT:
    case MTLC_INTRINSIC_GPU_FENCE_PROXY_ASYNC:
    case MTLC_INTRINSIC_GPU_TMA_LOAD_2D:
    case MTLC_INTRINSIC_GPU_TENSORMAP_ACQUIRE:
      mod_error(fn->m,
                "Metal: '%s' calls %s, an NVIDIA transaction-barrier or "
                "tensor-map operation; Apple GPUs have no such hardware. Use "
                "async_copy_workgroup or tensor_transfer",
                fn_name(fn), ir_intrinsic_name(intrinsic));
      return;
    default:
      mod_error(fn->m, "Metal: unsupported intrinsic %s in '%s'",
                ir_intrinsic_name(intrinsic), fn_name(fn));
      return;
    }
  }
  {
    size_t target = fn->spec && index < fn->spec->instruction_count
                        ? fn->spec->call_spec[index]
                        : SIZE_MAX;
    MslSpec *callee_spec = spec_of_index(fn->m, target);
    Sb args = {0};
    MslVar *dest = dest_var(fn, in);
    if (!callee_spec || !callee_spec->fn) {
      mod_error(fn->m, "Metal: '%s' calls '%s', which has no device "
                       "definition",
                fn_name(fn), in->text ? in->text : "?");
      return;
    }
    emit_call_args(fn, in, callee_spec, &args);
    if (dest && !callee_spec->fn->returns_void) {
      char expr[8192];
      MslClass ret = callee_spec->fn->ret_cls;
      if (dest->cls == MC_REC || dest->cls == MC_PTR) {
        snprintf(expr, sizeof(expr), "%s(%s)", callee_spec->name,
                 sb_str(&args));
        line(fn, "%s = %s;", dest->ident, expr);
      } else {
        if (ret == MC_NONE) {
          ret = MC_I32;
        }
        snprintf(expr, sizeof(expr), "(%s)%s(%s)", class_type(ret),
                 callee_spec->name, sb_str(&args));
        store_to_var(fn, dest, expr, ret);
      }
    } else {
      line(fn, "%s(%s);", callee_spec->name, sb_str(&args));
    }
    sb_free(&args);
  }
}

static void emit_barrier(MslFn *fn, const IRInstruction *in) {
  unsigned regions = in->memory_regions;
  char flags[128];
  if (regions == 0) {
    regions = MTLC_MEMORY_REGION_WORKGROUP;
  }
  flags[0] = '\0';
  if (regions & MTLC_MEMORY_REGION_WORKGROUP) {
    strcat(flags, "mem_flags::mem_threadgroup");
  }
  if (regions & MTLC_MEMORY_REGION_GLOBAL) {
    if (flags[0]) {
      strcat(flags, " | ");
    }
    strcat(flags, "mem_flags::mem_device");
  }
  if (in->memory_scope == MTLC_MEMORY_SCOPE_SUBGROUP) {
    line(fn, "simdgroup_barrier(%s);", flags);
  } else if (in->memory_scope == MTLC_MEMORY_SCOPE_WORKGROUP ||
             in->memory_scope == MTLC_MEMORY_SCOPE_DEFAULT) {
    line(fn, "threadgroup_barrier(%s);", flags);
  } else {
    mod_error(fn->m, "Metal: '%s' has a barrier wider than a workgroup",
              fn_name(fn));
  }
}

static void emit_async_copy(MslFn *fn, const IRInstruction *in) {
  MslSpace ds, ss;
  char d[2048], s[2048];
  MslVar *dv, *sv;
  MtlcTypeKind elem;
  size_t elem_size;
  if (in->argument_count != 2 || in->async_copy_element_count == 0) {
    mod_error(fn->m, "Metal: malformed asynchronous copy in '%s'",
              fn_name(fn));
    return;
  }
  dv = operand_var(fn, &in->arguments[0]);
  sv = operand_var(fn, &in->arguments[1]);
  elem = dv && dv->elem != MTLC_TYPE_VOID ? dv->elem
         : sv && sv->elem != MTLC_TYPE_VOID ? sv->elem
                                            : MTLC_TYPE_UINT32;
  elem_size = kind_size(elem);
  pointer_expr(fn, &in->arguments[0], &ds, d, sizeof(d));
  pointer_expr(fn, &in->arguments[1], &ss, s, sizeof(s));
  if (fn->reported) {
    char headline[120];
    snprintf(headline, sizeof(headline),
             "%zu bytes copied with ordinary assignments",
             (size_t)in->async_copy_element_count * elem_size);
    report_note(fn, in, "staging",
                in->async_copy_generated ? "promoted copy into workgroup memory"
                                         : "copy into workgroup memory",
                IR_DEVICE_FACT, headline, "Metal has no asynchronous copy",
                "async-copy");
  }
  if (in->async_copy_element_count <= 16) {
    for (uint32_t k = 0; k < in->async_copy_element_count; k++) {
      line(fn, "*(%s %s*)(%s + %zul) = *(%s %s*)(%s + %zul);",
           space_qual(fn->m, ds), access_type(elem), d,
           (size_t)k * elem_size, space_qual(fn->m, ss), access_type(elem), s,
           (size_t)k * elem_size);
    }
  } else {
    size_t id = fn->temp_counter++;
    line(fn, "for (uint mtl_k_%zu = 0u; mtl_k_%zu < %uu; mtl_k_%zu++) {", id,
         id, in->async_copy_element_count, id);
    fn->indent++;
    line(fn, "*(%s %s*)(%s + mtl_k_%zu * %zuu) = *(%s %s*)(%s + mtl_k_%zu * "
             "%zuu);",
         space_qual(fn->m, ds), access_type(elem), d, id, elem_size,
         space_qual(fn->m, ss), access_type(elem), s, id, elem_size);
    fn->indent--;
    line(fn, "}");
  }
}

typedef struct {
  char pointer[4][2048];
  MslSpace space[4];
  char stride[4][2048];
  int c_zero;
  char a_scale[2048];
  MslSpace a_scale_space;
  char b_scale[2048];
  MslSpace b_scale_space;
  char c_scale[2048];
  MslSpace c_scale_space;
} MslMmaOperands;

static const char *mma_element_type(MtlcTensorElement element) {
  switch (element) {
  case MTLC_TENSOR_ELEMENT_FLOAT16: return "half";
  case MTLC_TENSOR_ELEMENT_BFLOAT16: return "bfloat";
  case MTLC_TENSOR_ELEMENT_FLOAT32: return "float";
  case MTLC_TENSOR_ELEMENT_INT8: return "char";
  case MTLC_TENSOR_ELEMENT_UINT8: return "uchar";
  case MTLC_TENSOR_ELEMENT_INT32: return "int";
  default: return NULL;
  }
}

static size_t mma_element_size(MtlcTensorElement element) {
  switch (element) {
  case MTLC_TENSOR_ELEMENT_FLOAT16:
  case MTLC_TENSOR_ELEMENT_BFLOAT16:
    return 2;
  case MTLC_TENSOR_ELEMENT_FLOAT32:
  case MTLC_TENSOR_ELEMENT_INT32:
    return 4;
  case MTLC_TENSOR_ELEMENT_INT8:
  case MTLC_TENSOR_ELEMENT_UINT8:
    return 1;
  default:
    return 0;
  }
}

static int mma_collect(MslFn *fn, const IRInstruction *in, size_t base,
                       size_t per_tile, MslMmaOperands *ops) {
  const MtlcTensorMmaDesc *desc = &IR_TENSOR_MMA(in);
  size_t index = 4;
  unsigned mask = ir_tensor_mma_runtime_stride_mask(desc);
  unsigned long long fixed[4];
  memset(ops, 0, sizeof(*ops));
  fixed[0] = desc->a_leading_dimension;
  fixed[1] = desc->b_leading_dimension;
  fixed[2] = desc->c_leading_dimension;
  fixed[3] = desc->d_leading_dimension;
  ops->c_zero = ir_tensor_c_is_zero(&in->arguments[base + 2]);
  for (int i = 0; i < 4; i++) {
    if (i == 2 && ops->c_zero) {
      continue;
    }
    pointer_expr(fn, &in->arguments[base + i], &ops->space[i], ops->pointer[i],
                 sizeof(ops->pointer[i]));
    if (ops->space[i] == MS_THREAD) {
      mod_error(fn->m,
                "Metal: tensor_mma operand %d in '%s' points at private "
                "memory; simdgroup matrices load from device or threadgroup "
                "memory",
                i, fn_name(fn));
      return 0;
    }
  }
  if (desc->sparsity != MTLC_TENSOR_SPARSITY_DENSE) {
    index++;
  }
  if (desc->a_scale_mode != MTLC_TENSOR_SCALE_NONE) {
    pointer_expr(fn, &in->arguments[base + index++], &ops->a_scale_space,
                 ops->a_scale, sizeof(ops->a_scale));
  }
  if (desc->b_scale_mode != MTLC_TENSOR_SCALE_NONE) {
    pointer_expr(fn, &in->arguments[base + index++], &ops->b_scale_space,
                 ops->b_scale, sizeof(ops->b_scale));
  }
  for (int i = 0; i < 4; i++) {
    if (mask & (1u << i)) {
      materialize(fn, &in->arguments[base + index++], MC_U64, ops->stride[i],
                  sizeof(ops->stride[i]));
    } else {
      snprintf(ops->stride[i], sizeof(ops->stride[i]), "%lluul", fixed[i]);
    }
  }
  if (desc->c_scale_mode == MTLC_TENSOR_SCALE_PER_ROW) {
    pointer_expr(fn, &in->arguments[base + per_tile - 1], &ops->c_scale_space,
                 ops->c_scale, sizeof(ops->c_scale));
  }
  return 1;
}

static int mma_row_major(MtlcTensorLayout layout, int transpose) {
  return (layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR) != (transpose != 0);
}

static int mma_native_supported(MslFn *fn, const MtlcTensorMmaDesc *desc) {
  int float_input = desc->a_element == MTLC_TENSOR_ELEMENT_FLOAT16 ||
                    desc->a_element == MTLC_TENSOR_ELEMENT_FLOAT32 ||
                    (desc->a_element == MTLC_TENSOR_ELEMENT_BFLOAT16 &&
                     version_at_least(fn->m, 3, 1));
  int float_output = (desc->accumulator_element == MTLC_TENSOR_ELEMENT_FLOAT32 ||
                      desc->accumulator_element == MTLC_TENSOR_ELEMENT_FLOAT16) &&
                     (desc->result_element == MTLC_TENSOR_ELEMENT_FLOAT32 ||
                      desc->result_element == MTLC_TENSOR_ELEMENT_FLOAT16);
  return float_input && float_output && desc->b_element == desc->a_element &&
         desc->math_mode == MTLC_TENSOR_MATH_MULTIPLY_ADD &&
         desc->sparsity == MTLC_TENSOR_SPARSITY_DENSE &&
         desc->a_scale_mode == MTLC_TENSOR_SCALE_NONE &&
         desc->b_scale_mode == MTLC_TENSOR_SCALE_NONE &&
         desc->c_scale_mode == MTLC_TENSOR_SCALE_NONE &&
         desc->a_packing == MTLC_TENSOR_PACKING_LOGICAL &&
         desc->b_packing == MTLC_TENSOR_PACKING_LOGICAL &&
         desc->a_swizzle == 0 && desc->b_swizzle == 0 &&
         desc->a_zero_point == 0 && desc->b_zero_point == 0 &&
         desc->m % 8 == 0 && desc->n % 8 == 0 && desc->k % 8 == 0 &&
         desc->m > 0 && desc->n > 0 && desc->k > 0;
}

static void mma_block_pointer(MslFn *fn, const char *pointer, MslSpace space,
                              const char *type, size_t element_size,
                              const char *stride, int row_major,
                              const char *row, const char *col, char *out,
                              size_t size) {
  if (row_major) {
    snprintf(out, size, "(%s %s*)(%s + ((ulong)(%s) * %s + (ulong)(%s)) * %zuul)",
             space_qual(fn->m, space), type, pointer, row, stride, col,
             element_size);
  } else {
    snprintf(out, size, "(%s %s*)(%s + ((ulong)(%s) * %s + (ulong)(%s)) * %zuul)",
             space_qual(fn->m, space), type, pointer, col, stride, row,
             element_size);
  }
}

static const char *matrix_type(MtlcTensorElement element) {
  switch (element) {
  case MTLC_TENSOR_ELEMENT_FLOAT16: return "simdgroup_half8x8";
  case MTLC_TENSOR_ELEMENT_BFLOAT16: return "simdgroup_bfloat8x8";
  default: return "simdgroup_float8x8";
  }
}

static int resident_group(const MslFn *fn, uint32_t group) {
  for (size_t i = 0; i < fn->resident_count; i++) {
    if (fn->resident[i] == group) {
      return 1;
    }
  }
  return 0;
}

static void emit_mma_native_tile(MslFn *fn, const MtlcTensorMmaDesc *desc,
                                 const MslMmaOperands *ops, size_t id,
                                 int role, uint32_t group) {
  unsigned mb = desc->m / 8u, nb = desc->n / 8u, kb = desc->k / 8u;
  int resident = role != IR_TENSOR_RESIDENCY_NONE;
  char acc[64];
  const char *in_type = mma_element_type(desc->a_element);
  const char *acc_type = mma_element_type(desc->accumulator_element);
  const char *out_type = mma_element_type(desc->result_element);
  const char *acc_matrix = matrix_type(desc->accumulator_element);
  const char *in_matrix = matrix_type(desc->a_element);
  size_t in_size = mma_element_size(desc->a_element);
  size_t acc_size = mma_element_size(desc->accumulator_element);
  size_t out_size = mma_element_size(desc->result_element);
  int a_rows = mma_row_major(desc->a_layout, desc->transpose_a);
  int b_rows = mma_row_major(desc->b_layout, desc->transpose_b);
  int c_rows = desc->c_layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR;
  int d_rows = desc->d_layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR;
  char ptr[4800], row[64], col[64];
  if (resident) {
    snprintf(acc, sizeof(acc), "mtl_res_%u", (unsigned)group);
  } else {
    snprintf(acc, sizeof(acc), "mtl_acc_%zu", id);
  }
  line(fn, "{");
  fn->indent++;
  if (!resident) {
    line(fn, "%s %s[%u];", acc_matrix, acc, mb * nb);
  }
  line(fn, "%s mtl_ma_%zu[%u];", in_matrix, id, mb);
  line(fn, "%s mtl_mb_%zu[%u];", in_matrix, id, nb);
  for (unsigned i = 0; i < mb && role != IR_TENSOR_RESIDENCY_UPDATE; i++) {
    for (unsigned j = 0; j < nb; j++) {
      if (ops->c_zero) {
        line(fn, "%s[%u] = make_filled_simdgroup_matrix<%s, 8, 8>((%s)0);",
             acc, i * nb + j, acc_type, acc_type);
        continue;
      }
      snprintf(row, sizeof(row), "%uu", i * 8u);
      snprintf(col, sizeof(col), "%uu", j * 8u);
      mma_block_pointer(fn, ops->pointer[2], ops->space[2], acc_type, acc_size,
                        ops->stride[2], c_rows, row, col, ptr, sizeof(ptr));
      line(fn, "simdgroup_load(%s[%u], %s, %s, ulong2(0ul, 0ul), %s);", acc,
           i * nb + j, ptr, ops->stride[2], c_rows ? "false" : "true");
    }
  }
  if (kb > 4) {
    line(fn, "for (uint mtl_k_%zu = 0u; mtl_k_%zu < %uu; mtl_k_%zu++) {", id,
         id, kb, id);
    fn->indent++;
    snprintf(col, sizeof(col), "mtl_k_%zu * 8u", id);
  }
  for (unsigned k = 0; k < (kb > 4 ? 1u : kb); k++) {
    char kexpr[64];
    if (kb > 4) {
      snprintf(kexpr, sizeof(kexpr), "mtl_k_%zu * 8u", id);
    } else {
      snprintf(kexpr, sizeof(kexpr), "%uu", k * 8u);
    }
    for (unsigned i = 0; i < mb; i++) {
      snprintf(row, sizeof(row), "%uu", i * 8u);
      mma_block_pointer(fn, ops->pointer[0], ops->space[0], in_type, in_size,
                        ops->stride[0], a_rows, row, kexpr, ptr, sizeof(ptr));
      line(fn, "simdgroup_load(mtl_ma_%zu[%u], %s, %s, ulong2(0ul, 0ul), %s);", id,
           i, ptr, ops->stride[0], a_rows ? "false" : "true");
    }
    for (unsigned j = 0; j < nb; j++) {
      snprintf(col, sizeof(col), "%uu", j * 8u);
      mma_block_pointer(fn, ops->pointer[1], ops->space[1], in_type, in_size,
                        ops->stride[1], b_rows, kexpr, col, ptr, sizeof(ptr));
      line(fn, "simdgroup_load(mtl_mb_%zu[%u], %s, %s, ulong2(0ul, 0ul), %s);", id,
           j, ptr, ops->stride[1], b_rows ? "false" : "true");
    }
    for (unsigned i = 0; i < mb; i++) {
      for (unsigned j = 0; j < nb; j++) {
        line(fn,
             "simdgroup_multiply_accumulate(%s[%u], mtl_ma_%zu[%u], "
             "mtl_mb_%zu[%u], %s[%u]);",
             acc, i * nb + j, id, i, id, j, acc, i * nb + j);
      }
    }
  }
  if (kb > 4) {
    fn->indent--;
    line(fn, "}");
  }
  if (desc->result_element != desc->accumulator_element) {
    mod_error(fn->m,
              "Metal: tensor_mma in '%s' accumulates in one float format and "
              "stores another; simdgroup matrices store the accumulator's own "
              "format",
              fn_name(fn));
  }
  for (unsigned i = 0; i < mb && !resident; i++) {
    for (unsigned j = 0; j < nb; j++) {
      snprintf(row, sizeof(row), "%uu", i * 8u);
      snprintf(col, sizeof(col), "%uu", j * 8u);
      mma_block_pointer(fn, ops->pointer[3], ops->space[3], out_type, out_size,
                        ops->stride[3], d_rows, row, col, ptr, sizeof(ptr));
      line(fn, "simdgroup_store(%s[%u], %s, %s, ulong2(0ul, 0ul), %s);", acc,
           i * nb + j, ptr, ops->stride[3], d_rows ? "false" : "true");
    }
  }
  fn->indent--;
  line(fn, "}");
}

static void emit_mma_resident_commit(MslFn *fn, const MtlcTensorMmaDesc *desc,
                                     const MslMmaOperands *ops,
                                     uint32_t group) {
  unsigned mb = desc->m / 8u, nb = desc->n / 8u;
  const char *out_type = mma_element_type(desc->result_element);
  size_t out_size = mma_element_size(desc->result_element);
  int d_rows = desc->d_layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR;
  char ptr[4800], row[64], col[64];
  for (unsigned i = 0; i < mb; i++) {
    for (unsigned j = 0; j < nb; j++) {
      snprintf(row, sizeof(row), "%uu", i * 8u);
      snprintf(col, sizeof(col), "%uu", j * 8u);
      mma_block_pointer(fn, ops->pointer[3], ops->space[3], out_type, out_size,
                        ops->stride[3], d_rows, row, col, ptr, sizeof(ptr));
      line(fn, "simdgroup_store(mtl_res_%u[%u], %s, %s, ulong2(0ul, 0ul), %s);",
           (unsigned)group, i * nb + j, ptr, ops->stride[3],
           d_rows ? "false" : "true");
    }
  }
}

static void declare_residency(MslFn *fn) {
  const IRFunction *func = fn->func;
  for (size_t i = 0; i < func->instruction_count && !fn->m->error; i++) {
    const IRInstruction *in = &func->instructions[i];
    const MtlcTensorMmaDesc *desc;
    uint32_t *grown;
    if (in->op != IR_OP_TENSOR_MMA ||
        in->tensor_residency_role != IR_TENSOR_RESIDENCY_START ||
        in->tensor_residency_id == 0 ||
        ir_tensor_mma_instruction_count(in) != 1 ||
        resident_group(fn, in->tensor_residency_id)) {
      continue;
    }
    desc = &IR_TENSOR_MMA(in);
    if (!mma_native_supported(fn, desc) ||
        desc->result_element != desc->accumulator_element) {
      continue;
    }
    grown = realloc(fn->resident, (fn->resident_count + 1) * sizeof(uint32_t));
    if (!grown) {
      mod_error(fn->m, "Metal: out of memory");
      return;
    }
    fn->resident = grown;
    fn->resident[fn->resident_count++] = in->tensor_residency_id;
    line(fn, "%s mtl_res_%u[%u];", matrix_type(desc->accumulator_element),
         (unsigned)in->tensor_residency_id, (desc->m / 8u) * (desc->n / 8u));
  }
}

static int mma_scaled_supported(const MtlcTensorMmaDesc *desc) {
  int a_ok = desc->a_element == MTLC_TENSOR_ELEMENT_INT8 ||
             desc->a_element == MTLC_TENSOR_ELEMENT_UINT8;
  int b_ok = desc->b_element == MTLC_TENSOR_ELEMENT_INT8 ||
             desc->b_element == MTLC_TENSOR_ELEMENT_UINT8 ||
             desc->b_element == MTLC_TENSOR_ELEMENT_INT4 ||
             desc->b_element == MTLC_TENSOR_ELEMENT_UINT4;
  int scale_ok = (desc->a_scale_element == MTLC_TENSOR_ELEMENT_FLOAT32 ||
                  desc->a_scale_element == MTLC_TENSOR_ELEMENT_FLOAT16) &&
                 (desc->b_scale_element == MTLC_TENSOR_ELEMENT_FLOAT32 ||
                  desc->b_scale_element == MTLC_TENSOR_ELEMENT_FLOAT16);
  return a_ok && b_ok && scale_ok &&
         desc->a_scale_mode == MTLC_TENSOR_SCALE_BLOCK_32 &&
         desc->b_scale_mode == MTLC_TENSOR_SCALE_BLOCK_32 &&
         desc->sparsity == MTLC_TENSOR_SPARSITY_DENSE &&
         desc->math_mode == MTLC_TENSOR_MATH_MULTIPLY_ADD &&
         desc->accumulator_element == MTLC_TENSOR_ELEMENT_FLOAT32 &&
         desc->result_element == MTLC_TENSOR_ELEMENT_FLOAT32 &&
         desc->a_swizzle == 0 && desc->b_swizzle == 0 && desc->k % 32 == 0;
}

static void mma_operand_load(MslFn *fn, const char *pointer, MslSpace space,
                             MtlcTensorElement element,
                             MtlcTensorPacking packing, unsigned zero_point,
                             const char *stride, int row_major,
                             const char *row, const char *col, char *out,
                             size_t size) {
  char position[2400];
  if (row_major) {
    snprintf(position, sizeof(position), "((ulong)(%s) * %s + (ulong)(%s))",
             row, stride, col);
  } else {
    snprintf(position, sizeof(position), "((ulong)(%s) * %s + (ulong)(%s))",
             col, stride, row);
  }
  if (element == MTLC_TENSOR_ELEMENT_INT4 ||
      element == MTLC_TENSOR_ELEMENT_UINT4) {
    char byte[5000], high[2600], nibble[12000];
    if (packing == MTLC_TENSOR_PACKING_HALVES) {
      snprintf(byte, sizeof(byte), "(16ul * (%s / 32ul) + %s %% 16ul)",
               position, position);
      snprintf(high, sizeof(high), "(%s %% 32ul >= 16ul)", position);
    } else {
      snprintf(byte, sizeof(byte), "(%s / 2ul)", position);
      snprintf(high, sizeof(high), "((%s & 1ul) != 0ul)", position);
    }
    snprintf(nibble, sizeof(nibble),
             "(%s ? ((uint)*(%s uchar*)(%s + %s) >> 4) : ((uint)*(%s "
             "uchar*)(%s + %s) & 15u))",
             high, space_qual(fn->m, space), pointer, byte,
             space_qual(fn->m, space), pointer, byte);
    if (element == MTLC_TENSOR_ELEMENT_INT4) {
      snprintf(out, size, "(as_type<int>(%s) - (%s >= 8u ? 16 : 0))", nibble,
               nibble);
    } else {
      snprintf(out, size, "(as_type<int>(%s) - %u)", nibble, zero_point);
    }
    return;
  }
  if (element == MTLC_TENSOR_ELEMENT_INT8) {
    snprintf(out, size, "(int)*(%s char*)(%s + %s)", space_qual(fn->m, space),
             pointer, position);
    return;
  }
  snprintf(out, size, "((int)*(%s uchar*)(%s + %s) - %u)",
           space_qual(fn->m, space), pointer, position, zero_point);
}

static void mma_scale_load(MslFn *fn, const char *pointer, MslSpace space,
                           MtlcTensorElement element, const char *index,
                           char *out, size_t size) {
  if (element == MTLC_TENSOR_ELEMENT_FLOAT16) {
    snprintf(out, size, "(float)*(%s half*)(%s + (ulong)(%s) * 2ul)",
             space_qual(fn->m, space), pointer, index);
  } else {
    snprintf(out, size, "*(%s float*)(%s + (ulong)(%s) * 4ul)",
             space_qual(fn->m, space), pointer, index);
  }
}

static void emit_mma_scaled_tile(MslFn *fn, const MtlcTensorMmaDesc *desc,
                                 const MslMmaOperands *ops, size_t id) {
  IRBlockScaleOrder order = ir_tensor_block_scale_order(desc);
  unsigned blocks = desc->k / 32u;
  unsigned a_ld = desc->a_scale_leading_dimension
                      ? desc->a_scale_leading_dimension
                      : blocks;
  unsigned b_ld = desc->b_scale_leading_dimension
                      ? desc->b_scale_leading_dimension
                      : blocks;
  int a_rows = mma_row_major(desc->a_layout, desc->transpose_a);
  int b_rows = mma_row_major(desc->b_layout, desc->transpose_b);
  char x[12000], y[12000], index[256], sa[4000], sb[4000], tmp[4000];
  char r[64], c[64], kk[96];
  snprintf(r, sizeof(r), "mtl_r_%zu", id);
  snprintf(c, sizeof(c), "mtl_c_%zu", id);
  snprintf(kk, sizeof(kk), "mtl_kb_%zu * 32u + mtl_j_%zu", id, id);
  line(fn, "simdgroup_barrier(mem_flags::mem_threadgroup | mem_flags::mem_device);");
  line(fn, "for (uint mtl_o_%zu = mtl_lane; mtl_o_%zu < %uu; mtl_o_%zu += mtl_lanes) {",
       id, id, (unsigned)desc->m * desc->n, id);
  fn->indent++;
  line(fn, "uint mtl_r_%zu = mtl_o_%zu / %uu;", id, id, (unsigned)desc->n);
  line(fn, "uint mtl_c_%zu = mtl_o_%zu %% %uu;", id, id, (unsigned)desc->n);
  if (ops->c_zero) {
    line(fn, "float mtl_v_%zu = 0.0f;", id);
  } else {
    line(fn, "float mtl_v_%zu = *(%s float*)(%s + ((ulong)(%s) * %s + (ulong)(%s)) * 4ul);",
         id, space_qual(fn->m, ops->space[2]), ops->pointer[2],
         desc->c_layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR ? r : c,
         ops->stride[2],
         desc->c_layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR ? c : r);
  }
  if (desc->c_scale_mode == MTLC_TENSOR_SCALE_PER_ROW) {
    line(fn, "mtl_v_%zu = mtl_v_%zu * *(%s float*)(%s + (ulong)(%s) * 4ul);", id,
         id, space_qual(fn->m, ops->c_scale_space), ops->c_scale, r);
  }
  line(fn, "for (uint mtl_kb_%zu = 0u; mtl_kb_%zu < %uu; mtl_kb_%zu++) {", id,
       id, blocks, id);
  fn->indent++;
  line(fn, "int mtl_dot_%zu = 0;", id);
  line(fn, "for (uint mtl_j_%zu = 0u; mtl_j_%zu < 32u; mtl_j_%zu++) {", id, id,
       id);
  fn->indent++;
  mma_operand_load(fn, ops->pointer[0], ops->space[0], desc->a_element,
                   desc->a_packing, desc->a_zero_point, ops->stride[0], a_rows,
                   r, kk, x, sizeof(x));
  mma_operand_load(fn, ops->pointer[1], ops->space[1], desc->b_element,
                   desc->b_packing, desc->b_zero_point, ops->stride[1], b_rows,
                   kk, c, y, sizeof(y));
  line(fn, "mtl_dot_%zu = as_type<int>(as_type<uint>(mtl_dot_%zu) + as_type<uint>(%s * %s));",
       id, id, x, y);
  fn->indent--;
  line(fn, "}");
  snprintf(index, sizeof(index), "%s * %uu + mtl_kb_%zu", r, a_ld, id);
  mma_scale_load(fn, ops->a_scale, ops->a_scale_space, desc->a_scale_element,
                 index, sa, sizeof(sa));
  snprintf(index, sizeof(index), "%s * %uu + mtl_kb_%zu", c, b_ld, id);
  mma_scale_load(fn, ops->b_scale, ops->b_scale_space, desc->b_scale_element,
                 index, sb, sizeof(sb));
  line(fn, "float mtl_sa_%zu = %s;", id, sa);
  line(fn, "float mtl_sb_%zu = %s;", id, sb);
  line(fn, "float mtl_f_%zu = 12582912.0f + (float)(mtl_dot_%zu);", id, id);
  if (order == IR_BLOCK_SCALE_B_FIRST) {
    snprintf(tmp, sizeof(tmp),
             "fma(mtl_f_%zu, mtl_sb_%zu, -(12582912.0f * mtl_sb_%zu))", id, id,
             id);
    line(fn, "mtl_v_%zu = fma(%s, mtl_sa_%zu, mtl_v_%zu);", id, tmp, id, id);
  } else if (order == IR_BLOCK_SCALE_A_FIRST) {
    snprintf(tmp, sizeof(tmp),
             "fma(mtl_f_%zu, mtl_sa_%zu, -(12582912.0f * mtl_sa_%zu))", id, id,
             id);
    line(fn, "mtl_v_%zu = fma(%s, mtl_sb_%zu, mtl_v_%zu);", id, tmp, id, id);
  } else {
    line(fn,
         "mtl_v_%zu = fma(mtl_f_%zu - 12582912.0f, mtl_sa_%zu * mtl_sb_%zu, "
         "mtl_v_%zu);",
         id, id, id, id, id);
  }
  fn->indent--;
  line(fn, "}");
  line(fn, "*(%s float*)(%s + ((ulong)(%s) * %s + (ulong)(%s)) * 4ul) = mtl_v_%zu;",
       space_qual(fn->m, ops->space[3]), ops->pointer[3],
       desc->d_layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR ? r : c, ops->stride[3],
       desc->d_layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR ? c : r, id);
  fn->indent--;
  line(fn, "}");
  line(fn, "simdgroup_barrier(mem_flags::mem_threadgroup | mem_flags::mem_device);");
}

static void emit_tensor_mma(MslFn *fn, const IRInstruction *in) {
  const MtlcTensorMmaDesc *desc = &IR_TENSOR_MMA(in);
  size_t per_tile = ir_tensor_mma_operand_count(desc);
  size_t tiles = ir_tensor_mma_instruction_count(in);
  int native = mma_native_supported(fn, desc);
  int scaled = !native && mma_scaled_supported(desc);
  if (!per_tile || in->argument_count != per_tile * tiles) {
    mod_error(fn->m, "Metal: malformed tensor_mma in '%s'", fn_name(fn));
    return;
  }
  if (!native && !scaled) {
    mod_error(fn->m,
              "Metal: tensor_mma in '%s' uses a format Apple GPUs have no "
              "matrix unit for and the Metal backend does not replay yet; "
              "Metal runs dense f16, bf16 and f32 MMA on simdgroup matrices "
              "and block-scaled int8 x int8/int4 exactly in scalar code",
              fn_name(fn));
    return;
  }
  if (fn->reported) {
    char subject[64];
    snprintf(subject, sizeof(subject), "tensor_mma m%un%uk%u",
             (unsigned)desc->m, (unsigned)desc->n, (unsigned)desc->k);
    if (native) {
      report_note(fn, in, "tensor", subject, IR_DEVICE_GOOD,
                  "simdgroup_matrix 8x8 tiles", NULL, "tensor-instruction");
    } else {
      report_note(fn, in, "tensor", subject, IR_DEVICE_FACT,
                  "exact integer dot products in scalar code",
                  "Apple GPUs have no integer matrix unit",
                  "tensor-scalar");
    }
    if (native && tiles == 1 &&
        (in->tensor_residency_role == IR_TENSOR_RESIDENCY_START ||
         in->tensor_residency_role == IR_TENSOR_RESIDENCY_UPDATE) &&
        resident_group(fn, in->tensor_residency_id)) {
      report_note(fn, in, "tensor", subject, IR_DEVICE_GOOD,
                  "the accumulator stays in simdgroup matrices across the "
                  "region",
                  NULL, "tensor-resident");
    }
  }
  if (native) {
    line(fn, "simdgroup_barrier(mem_flags::mem_threadgroup | mem_flags::mem_device);");
  }
  for (size_t tile = 0; tile < tiles && !fn->m->error; tile++) {
    MslMmaOperands ops;
    size_t id = fn->temp_counter++;
    int role = IR_TENSOR_RESIDENCY_NONE;
    if (!mma_collect(fn, in, tile * per_tile, per_tile, &ops)) {
      return;
    }
    if (native && tiles == 1 &&
        (in->tensor_residency_role == IR_TENSOR_RESIDENCY_START ||
         in->tensor_residency_role == IR_TENSOR_RESIDENCY_UPDATE) &&
        resident_group(fn, in->tensor_residency_id)) {
      role = (int)in->tensor_residency_role;
    }
    if (native) {
      emit_mma_native_tile(fn, desc, &ops, id, role, in->tensor_residency_id);
    } else {
      emit_mma_scaled_tile(fn, desc, &ops, id);
    }
  }
  if (native) {
    line(fn, "simdgroup_barrier(mem_flags::mem_threadgroup | mem_flags::mem_device);");
  }
}

static void emit_scale_rows_resident(MslFn *fn, const MtlcTensorMmaDesc *desc,
                                     const char *scale, MslSpace scale_space,
                                     size_t id, uint32_t group) {
  unsigned mb = desc->m / 8u, nb = desc->n / 8u;
  int half = desc->accumulator_element == MTLC_TENSOR_ELEMENT_FLOAT16;
  line(fn, "{");
  fn->indent++;
  line(fn, "uint mtl_srow_%zu = ((mtl_lane / 4u) & 4u) + (mtl_lane / 2u) %% 4u;",
       id);
  for (unsigned i = 0; i < mb; i++) {
    line(fn, "float mtl_s_%zu_%u = *(%s float*)(%s + (ulong)(%uu + mtl_srow_%zu) * 4ul);",
         id, i, space_qual(fn->m, scale_space), scale, i * 8u, id);
    for (unsigned j = 0; j < nb; j++) {
      for (unsigned e = 0; e < 2 && half; e++) {
        line(fn,
             "mtl_res_%u[%u].thread_elements()[%u] = (half)((float)mtl_res_%u[%u]"
             ".thread_elements()[%u] * mtl_s_%zu_%u);",
             (unsigned)group, i * nb + j, e, (unsigned)group, i * nb + j, e,
             id, i);
      }
      for (unsigned e = 0; e < 2 && !half; e++) {
        line(fn, "mtl_res_%u[%u].thread_elements()[%u] *= mtl_s_%zu_%u;",
             (unsigned)group, i * nb + j, e, id, i);
      }
    }
  }
  fn->indent--;
  line(fn, "}");
}

static void emit_scale_rows_memory(MslFn *fn, const MtlcTensorMmaDesc *desc,
                                   const IRInstruction *in, const char *scale,
                                   MslSpace scale_space, size_t id) {
  char d[2048], stride[2048];
  MslSpace d_space;
  int row_major = desc->d_layout == MTLC_TENSOR_LAYOUT_ROW_MAJOR;
  const char *acc_type = mma_element_type(desc->accumulator_element);
  pointer_expr(fn, &in->arguments[0], &d_space, d, sizeof(d));
  if (in->argument_count > 2) {
    materialize(fn, &in->arguments[2], MC_U64, stride, sizeof(stride));
  } else {
    snprintf(stride, sizeof(stride), "%uul",
             (unsigned)desc->d_leading_dimension);
  }
  line(fn, "for (uint mtl_o_%zu = mtl_lane; mtl_o_%zu < %uu; mtl_o_%zu += mtl_lanes) {",
       id, id, (unsigned)desc->m * desc->n, id);
  fn->indent++;
  line(fn, "uint mtl_r_%zu = mtl_o_%zu / %uu;", id, id, (unsigned)desc->n);
  line(fn, "uint mtl_c_%zu = mtl_o_%zu %% %uu;", id, id, (unsigned)desc->n);
  line(fn, "%s %s* mtl_p_%zu = (%s %s*)(%s + ((ulong)(mtl_%c_%zu) * %s + (ulong)(mtl_%c_%zu)) * %zuul);",
       space_qual(fn->m, d_space), acc_type, id, space_qual(fn->m, d_space),
       acc_type, d, row_major ? 'r' : 'c', id, stride, row_major ? 'c' : 'r',
       id, mma_element_size(desc->accumulator_element));
  line(fn, "*mtl_p_%zu = (%s)((float)*mtl_p_%zu * *(%s float*)(%s + (ulong)(mtl_r_%zu) * 4ul));",
       id, acc_type, id, space_qual(fn->m, scale_space), scale, id);
  fn->indent--;
  line(fn, "}");
}

static void emit_tensor_scale_rows(MslFn *fn, const IRInstruction *in) {
  const MtlcTensorMmaDesc *desc = &IR_TENSOR_MMA(in);
  char scale[2048];
  MslSpace scale_space;
  size_t id = fn->temp_counter++;
  int resident = in->tensor_residency_role == IR_TENSOR_RESIDENCY_UPDATE &&
                 resident_group(fn, in->tensor_residency_id);
  if (in->argument_count != ir_tensor_scale_rows_operand_count(desc) ||
      !mma_element_type(desc->accumulator_element)) {
    mod_error(fn->m, "Metal: malformed tensor_scale_rows in '%s'",
              fn_name(fn));
    return;
  }
  pointer_expr(fn, &in->arguments[1], &scale_space, scale, sizeof(scale));
  if (scale_space == MS_THREAD) {
    mod_error(fn->m,
              "Metal: tensor_scale_rows in '%s' reads its row scales from "
              "private memory; put them in threadgroup or device memory",
              fn_name(fn));
    return;
  }
  line(fn, "simdgroup_barrier(mem_flags::mem_threadgroup | mem_flags::mem_device);");
  if (resident) {
    emit_scale_rows_resident(fn, desc, scale, scale_space, id,
                             in->tensor_residency_id);
  } else {
    emit_scale_rows_memory(fn, desc, in, scale, scale_space, id);
  }
  line(fn, "simdgroup_barrier(mem_flags::mem_threadgroup | mem_flags::mem_device);");
}

static void emit_instruction(MslFn *fn, const IRInstruction *in,
                             size_t index) {
  if (fn->m->error) {
    return;
  }
  fn->m->current = in;
  switch (in->op) {
  case IR_OP_NOP:
  case IR_OP_LABEL:
  case IR_OP_DECLARE_LOCAL:
  case IR_OP_ADDRESS_SPACE_ALLOC:
  case IR_OP_ASYNC_COMMIT:
  case IR_OP_ASYNC_WAIT:
  case IR_OP_PREFETCH:
    return;
  case IR_OP_BARRIER:
    emit_barrier(fn, in);
    return;
  case IR_OP_ASYNC_COPY:
    emit_async_copy(fn, in);
    return;
  case IR_OP_ASSIGN: {
    MslVar *dest = dest_var(fn, in);
    char x[2048];
    if (!dest) {
      return;
    }
    if (dest->cls == MC_REC) {
      MslVar *source = operand_var(fn, &in->lhs);
      if (!source || source->cls != MC_REC) {
        mod_error(fn->m, "Metal: '%s' assigns a non-record to record '%s'",
                  fn_name(fn), source_name(dest->name));
        return;
      }
      line(fn, "%s = %s;", dest->ident, source->ident);
      return;
    }
    materialize(fn, &in->lhs, dest->cls, x, sizeof(x));
    store_dest(fn, in, x, dest->cls);
    return;
  }
  case IR_OP_BINARY:
    emit_binary(fn, in);
    return;
  case IR_OP_UNARY:
    emit_unary(fn, in);
    return;
  case IR_OP_LOAD:
    emit_load(fn, in);
    return;
  case IR_OP_STORE:
    emit_store(fn, in);
    return;
  case IR_OP_CAST:
    emit_cast(fn, in);
    return;
  case IR_OP_SELECT:
    emit_select(fn, in);
    return;
  case IR_OP_ADDRESS_OF:
    emit_address_of(fn, in);
    return;
  case IR_OP_CALL:
    emit_call(fn, in, index);
    return;
  case IR_OP_INLINE_ASM:
  case IR_OP_ASM_RESULT:
    mod_error(fn->m,
              "Metal: '%s' has an inline PTX block; PTX has no meaning on "
              "Apple GPUs, so write the operation in Mettle or keep it in a "
              "PTX-only kernel file",
              fn_name(fn));
    return;
  case IR_OP_TENSOR_MMA:
    emit_tensor_mma(fn, in);
    return;
  case IR_OP_TENSOR_SCALE_ROWS:
    emit_tensor_scale_rows(fn, in);
    return;
  case IR_OP_TENSOR_COMMIT:
    if (resident_group(fn, in->tensor_residency_id)) {
      MslMmaOperands ops;
      size_t per_tile = ir_tensor_mma_operand_count(&IR_TENSOR_MMA(in));
      if (!per_tile || in->argument_count < per_tile ||
          !mma_collect(fn, in, 0, per_tile, &ops)) {
        mod_error(fn->m, "Metal: malformed tensor commit in '%s'",
                  fn_name(fn));
        return;
      }
      line(fn, "simdgroup_barrier(mem_flags::mem_threadgroup | "
               "mem_flags::mem_device);");
      emit_mma_resident_commit(fn, &IR_TENSOR_MMA(in), &ops,
                               in->tensor_residency_id);
      line(fn, "simdgroup_barrier(mem_flags::mem_threadgroup | "
               "mem_flags::mem_device);");
    }
    return;
  case IR_OP_TENSOR_MATMUL:
  case IR_OP_TENSOR_EPILOGUE:
  case IR_OP_TENSOR_TRANSFER:
  case IR_OP_TILE:
    mod_error(fn->m,
              "Metal: '%s' uses a cooperative tensor operation, which the "
              "Metal backend does not lower yet",
              fn_name(fn));
    return;
  default:
    mod_error(fn->m, "Metal: '%s' has IR opcode %s, which device code on "
                     "Metal does not support",
              fn_name(fn), ir_opcode_name(in->op));
    return;
  }
}

static int is_terminator(IROpcode op) {
  return op == IR_OP_JUMP || op == IR_OP_BRANCH_ZERO ||
         op == IR_OP_BRANCH_EQ || op == IR_OP_RETURN;
}

static void split_blocks(MslFn *fn) {
  IRFunction *func = fn->func;
  size_t count = func->instruction_count;
  size_t *starts = calloc(count + 2, sizeof(size_t));
  size_t nstarts = 0;
  if (!starts) {
    mod_error(fn->m, "Metal: out of memory");
    return;
  }
  starts[nstarts++] = 0;
  for (size_t i = 0; i < count; i++) {
    IROpcode op = func->instructions[i].op;
    if (op == IR_OP_LABEL && starts[nstarts - 1] != i) {
      starts[nstarts++] = i;
    }
    if (is_terminator(op) && i + 1 < count && starts[nstarts - 1] != i + 1) {
      starts[nstarts++] = i + 1;
    }
  }
  fn->blocks = calloc(nstarts + 1, sizeof(MslBlock));
  if (!fn->blocks) {
    free(starts);
    mod_error(fn->m, "Metal: out of memory");
    return;
  }
  for (size_t k = 0; k < nstarts; k++) {
    size_t lo = starts[k];
    size_t hi = k + 1 < nstarts ? starts[k + 1] : count;
    MslBlock *block;
    if (lo >= hi) {
      continue;
    }
    block = &fn->blocks[fn->nblocks++];
    block->lo = lo;
    block->hi = hi;
    block->label =
        func->instructions[lo].op == IR_OP_LABEL ? func->instructions[lo].text
                                                 : NULL;
    block->term = is_terminator(func->instructions[hi - 1].op)
                      ? &func->instructions[hi - 1]
                      : NULL;
  }
  if (fn->nblocks == 0) {
    fn->nblocks = 1;
    fn->blocks[0].lo = 0;
    fn->blocks[0].hi = 0;
  }
  free(starts);
}

static long block_of_label(MslFn *fn, const char *label) {
  if (!label) {
    return -1;
  }
  for (size_t i = 0; i < fn->nblocks; i++) {
    if (fn->blocks[i].label && !strcmp(fn->blocks[i].label, label)) {
      return (long)i;
    }
  }
  return -1;
}

static void print_condition(MslFn *fn, const IRInstruction *term,
                            int negate, char *out, size_t size) {
  char x[2048], y[2048];
  const char *op = negate ? "!=" : "==";
  if (term->op == IR_OP_BRANCH_ZERO) {
    MslClass c = operand_class(fn, &term->lhs);
    if (c == MC_F32) {
      materialize(fn, &term->lhs, MC_F32, x, sizeof(x));
      snprintf(out, size, "%s %s 0.0f", x, op);
    } else if (c == MC_PTR) {
      materialize(fn, &term->lhs, MC_PTR, x, sizeof(x));
      snprintf(out, size, "%s %s nullptr", x, op);
    } else {
      materialize(fn, &term->lhs, class_is_64(c) ? MC_U64 : MC_U32, x,
                  sizeof(x));
      snprintf(out, size, "%s %s 0%s", x, op, class_is_64(c) ? "ul" : "u");
    }
    return;
  }
  {
    MslClass a = operand_class(fn, &term->lhs);
    MslClass b = operand_class(fn, &term->rhs);
    MslClass c = (a == MC_F32 || b == MC_F32) ? MC_F32
                 : (class_is_64(a) || class_is_64(b)) ? MC_U64
                                                      : MC_U32;
    if (a == MC_PTR && b == MC_PTR) {
      c = MC_PTR;
    }
    materialize(fn, &term->lhs, c, x, sizeof(x));
    materialize(fn, &term->rhs, c, y, sizeof(y));
    snprintf(out, size, "%s %s %s", x, op, y);
  }
}

static int block_prints_nothing(MslFn *fn, size_t b) {
  MslBlock *block = &fn->blocks[b];
  for (size_t i = block->lo; i < block->hi; i++) {
    const IRInstruction *in = &fn->func->instructions[i];
    if (in == block->term) {
      break;
    }
    switch (in->op) {
    case IR_OP_NOP:
    case IR_OP_LABEL:
    case IR_OP_DECLARE_LOCAL:
    case IR_OP_ADDRESS_SPACE_ALLOC:
    case IR_OP_ASYNC_COMMIT:
    case IR_OP_ASYNC_WAIT:
    case IR_OP_PREFETCH:
    case IR_OP_TENSOR_COMMIT:
      break;
    default:
      return 0;
    }
  }
  return 1;
}

static int seq_prints_nothing(MslFn *fn, const GpuSNode *seq) {
  if (!seq) {
    return 1;
  }
  for (size_t i = 0; i < seq->child_count; i++) {
    const GpuSNode *child = seq->children[i];
    if (child->kind != GPU_SNODE_CODE || !block_prints_nothing(fn, child->block)) {
      return 0;
    }
  }
  return 1;
}

static int seq_always_leaves(const GpuSNode *seq) {
  const GpuSNode *last;
  if (!seq || seq->child_count == 0) {
    return 0;
  }
  last = seq->children[seq->child_count - 1];
  if (last->kind == GPU_SNODE_BREAK || last->kind == GPU_SNODE_CONTINUE ||
      last->kind == GPU_SNODE_RETURN) {
    return 1;
  }
  if (last->kind == GPU_SNODE_IF) {
    return seq_always_leaves(last->then_node) &&
           seq_always_leaves(last->else_node);
  }
  return 0;
}

static void emit_block_code(MslFn *fn, size_t b) {
  MslBlock *block = &fn->blocks[b];
  for (size_t i = block->lo; i < block->hi && !fn->m->error; i++) {
    const IRInstruction *in = &fn->func->instructions[i];
    if (in == block->term) {
      break;
    }
    emit_instruction(fn, in, i);
  }
}

static void emit_return(MslFn *fn, size_t b) {
  const IRInstruction *term = fn->blocks[b].term;
  char x[2048];
  if (!term || term->op != IR_OP_RETURN ||
      term->lhs.kind == IR_OPERAND_NONE) {
    if (!fn->returns_void) {
      mod_error(fn->m, "Metal: '%s' can reach its end without a value",
                fn_name(fn));
      return;
    }
    line(fn, "return;");
    return;
  }
  if (fn->returns_void) {
    line(fn, "return;");
    return;
  }
  if (fn->ret_cls == MC_REC || fn->ret_cls == MC_PTR) {
    MslVar *var = operand_var(fn, &term->lhs);
    if (fn->ret_cls == MC_PTR && term->lhs.kind == IR_OPERAND_INT) {
      line(fn, "return nullptr;");
      return;
    }
    line(fn, "return %s;", var ? var->ident : "{}");
    return;
  }
  materialize(fn, &term->lhs, fn->ret_cls, x, sizeof(x));
  if (fn->ret_kind == MTLC_TYPE_BOOL) {
    line(fn, "return (uchar)((%s) != 0u);", x);
    return;
  }
  line(fn, "return (%s)(%s);",
       storage_type(fn->ret_kind) ? storage_type(fn->ret_kind)
                                  : class_type(fn->ret_cls),
       x);
}

static void emit_tree(MslFn *fn, const GpuSNode *node);

static void emit_seq(MslFn *fn, const GpuSNode *seq) {
  if (!seq) {
    return;
  }
  for (size_t i = 0; i < seq->child_count && !fn->m->error; i++) {
    emit_tree(fn, seq->children[i]);
  }
}

static void emit_tree(MslFn *fn, const GpuSNode *node) {
  switch (node->kind) {
  case GPU_SNODE_SEQ:
    emit_seq(fn, node);
    return;
  case GPU_SNODE_CODE:
    emit_block_code(fn, node->block);
    return;
  case GPU_SNODE_IF: {
    char cond[4096];
    const IRInstruction *term = fn->blocks[node->block].term;
    int then_empty = seq_prints_nothing(fn, node->then_node);
    int else_empty = seq_prints_nothing(fn, node->else_node);
    if (!term) {
      mod_error(fn->m, "Metal: branch without a condition in '%s'",
                fn_name(fn));
      return;
    }
    if (then_empty && else_empty) {
      return;
    }
    if (then_empty) {
      print_condition(fn, term, 1, cond, sizeof(cond));
      line(fn, "if (%s) {", cond);
      fn->indent++;
      emit_seq(fn, node->else_node);
      fn->indent--;
      line(fn, "}");
      return;
    }
    print_condition(fn, term, 0, cond, sizeof(cond));
    line(fn, "if (%s) {", cond);
    fn->indent++;
    emit_seq(fn, node->then_node);
    fn->indent--;
    if (!else_empty && seq_always_leaves(node->then_node)) {
      line(fn, "}");
      emit_seq(fn, node->else_node);
      return;
    }
    if (!else_empty) {
      line(fn, "} else {");
      fn->indent++;
      emit_seq(fn, node->else_node);
      fn->indent--;
    }
    line(fn, "}");
    return;
  }
  case GPU_SNODE_LOOP:
    line(fn, "while (true) {");
    fn->indent++;
    emit_seq(fn, node->body);
    fn->indent--;
    line(fn, "}");
    return;
  case GPU_SNODE_BLOCK:
    line(fn, "switch (0) {");
    line(fn, "default: {");
    fn->indent++;
    emit_seq(fn, node->body);
    fn->indent--;
    line(fn, "}");
    line(fn, "}");
    return;
  case GPU_SNODE_BREAK:
    line(fn, "break;");
    return;
  case GPU_SNODE_CONTINUE:
    line(fn, "continue;");
    return;
  case GPU_SNODE_RETURN:
    emit_return(fn, node->block);
    return;
  case GPU_SNODE_SET_EXIT:
    line(fn, "mtl_exit = %d;", node->value);
    return;
  case GPU_SNODE_CHECK:
    for (size_t i = 0; i < node->case_count; i++) {
      const GpuExitCase *c = &node->cases[i];
      if (c->action == GPU_EXIT_BREAK_KEEP) {
        line(fn, "if (mtl_exit == %d) {", c->value);
        fn->indent++;
        line(fn, "break;");
      } else {
        line(fn, "if (mtl_exit == %d) {", c->value);
        fn->indent++;
        line(fn, "mtl_exit = 0;");
        line(fn, c->action == GPU_EXIT_CONTINUE_CLEAR ? "continue;"
                                                      : "break;");
      }
      fn->indent--;
      line(fn, "}");
    }
    return;
  default:
    mod_error(fn->m, "Metal: unexpected structured node in '%s'",
              fn_name(fn));
    return;
  }
}

static int structure_function(MslFn *fn, GpuStructure *structure) {
  GpuCfgNode *cfg;
  char error[512];
  split_blocks(fn);
  if (fn->m->error) {
    return 0;
  }
  cfg = calloc(fn->nblocks + 1, sizeof(GpuCfgNode));
  if (!cfg) {
    mod_error(fn->m, "Metal: out of memory");
    return 0;
  }
  for (size_t b = 0; b < fn->nblocks; b++) {
    const IRInstruction *term = fn->blocks[b].term;
    size_t next = b + 1;
    if (term && term->op == IR_OP_RETURN) {
      cfg[b].exit = GPU_CFG_RETURN;
    } else if (term && term->op == IR_OP_JUMP) {
      long target = block_of_label(fn, term->text);
      if (target < 0) {
        mod_error(fn->m, "Metal: jump to unknown label '%s' in '%s'",
                  term->text ? term->text : "?", fn_name(fn));
        free(cfg);
        return 0;
      }
      cfg[b].exit = GPU_CFG_JUMP;
      cfg[b].target[0] = cfg[b].target[1] = (size_t)target;
    } else if (term) {
      long target = block_of_label(fn, term->text);
      if (target < 0) {
        mod_error(fn->m, "Metal: branch to unknown label '%s' in '%s'",
                  term->text ? term->text : "?", fn_name(fn));
        free(cfg);
        return 0;
      }
      if (next >= fn->nblocks) {
        cfg[b].exit = GPU_CFG_JUMP;
        cfg[b].target[0] = cfg[b].target[1] = (size_t)target;
      } else {
        cfg[b].exit = GPU_CFG_BRANCH;
        cfg[b].target[0] = (size_t)target;
        cfg[b].target[1] = next;
      }
    } else if (next < fn->nblocks) {
      cfg[b].exit = GPU_CFG_JUMP;
      cfg[b].target[0] = cfg[b].target[1] = next;
    } else {
      cfg[b].exit = GPU_CFG_RETURN;
    }
  }
  if (!gpu_structure_build(cfg, fn->nblocks, 0, structure, error,
                           sizeof(error))) {
    mod_error(fn->m, "Metal: '%s' has control flow Metal cannot express: %s",
              fn_name(fn), error);
    free(cfg);
    return 0;
  }
  free(cfg);
  return 1;
}

static void declare_locals(MslFn *fn) {
  IRFunction *func = fn->func;
  for (size_t i = 0; i < fn->nvars && !fn->m->error; i++) {
    MslVar *var = &fn->vars[i];
    (void)func;
    if (var->is_param) {
      continue;
    }
    if (var->alloc == 1) {
      const char *type = var->alloc_elem_type && type_is_record(var->alloc_elem_type)
                             ? record_name(fn->m, var->alloc_elem_type)
                             : storage_type(var->alloc_elem);
      if (!type || var->alloc_elem == MTLC_TYPE_FLOAT64) {
        mod_error(fn->m, "Metal: '%s' allocates '%s' with an element type "
                         "Metal cannot hold",
                  fn_name(fn), source_name(var->name));
        return;
      }
      line(fn, "alignas(16) %s %s mtl_s_%s[%lld];", space_word(var->alloc_space),
           type, var->ident, var->alloc_count > 0 ? var->alloc_count : 1);
      line(fn, "%s uchar* %s = (%s uchar*)mtl_s_%s;",
           space_word(var->alloc_space), var->ident,
           space_word(var->alloc_space), var->ident);
      continue;
    }
    if (var->alloc == 2) {
      line(fn, "threadgroup uchar* %s = mtl_arena;", var->ident);
      continue;
    }
    if (var->cls == MC_PTR) {
      line(fn, "%s uchar* %s = nullptr;", space_qual(fn->m, var->space),
           var->ident);
    } else if (var->cls == MC_REC) {
      line(fn, "%s %s = {};", record_name(fn->m, var->rec), var->ident);
    } else {
      const char *type = var->storage != MTLC_TYPE_VOID
                             ? storage_type(var->storage)
                             : class_type(var->cls);
      if (var->cls == MC_F64 || var->storage == MTLC_TYPE_FLOAT64) {
        check_f64(fn, MC_F64);
        return;
      }
      if ((var->storage == MTLC_TYPE_BFLOAT16) &&
          !version_at_least(fn->m, 3, 1)) {
        mod_error(fn->m, "Metal: bfloat16 needs --metal-version=3.1 or later");
        return;
      }
      line(fn, "%s %s = (%s)0;", type ? type : "int", var->ident,
           type ? type : "int");
    }
  }
}

static void param_decl(MslFn *fn, MslVar *var, Sb *out) {
  if (var->cls == MC_PTR) {
    sb_printf(out, "%s uchar* %s", space_qual(fn->m, var->space), var->ident);
  } else if (var->cls == MC_REC) {
    sb_printf(out, "%s %s", record_name(fn->m, var->rec), var->ident);
  } else {
    const char *type = var->storage != MTLC_TYPE_VOID
                           ? storage_type(var->storage)
                           : class_type(var->cls);
    sb_printf(out, "%s %s", type ? type : "int", var->ident);
  }
}

static void builtin_params(unsigned builtins, Sb *out, int *first,
                           int kernel) {
  static const struct {
    unsigned bit;
    const char *type;
    const char *name;
    const char *attribute;
  } table[] = {
      {MB_TID, "uint3", "mtl_tid", "thread_position_in_threadgroup"},
      {MB_CTAID, "uint3", "mtl_ctaid", "threadgroup_position_in_grid"},
      {MB_NTID, "uint3", "mtl_ntid", "threads_per_threadgroup"},
      {MB_NCTAID, "uint3", "mtl_nctaid", "threadgroups_per_grid"},
      {MB_LANE, "uint", "mtl_lane", "thread_index_in_simdgroup"},
      {MB_LANES, "uint", "mtl_lanes", "threads_per_simdgroup"}};
  for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
    if (!(builtins & table[i].bit)) {
      continue;
    }
    if (!*first) {
      sb_puts(out, kernel ? ",\n    " : ", ");
    }
    *first = 0;
    if (kernel) {
      sb_printf(out, "%s %s [[%s]]", table[i].type, table[i].name,
                table[i].attribute);
    } else {
      sb_printf(out, "%s %s", table[i].type, table[i].name);
    }
  }
}

static void helper_signature(MslMod *m, MslSpec *spec, Sb *out) {
  MslFn *fn = spec->fn;
  IRFunction *func = fn->func;
  int first = 1;
  if (fn->returns_void) {
    sb_puts(out, "static void ");
  } else if (fn->ret_cls == MC_REC) {
    sb_printf(out, "static %s ", record_name(m, fn->ret_rec));
  } else if (fn->ret_cls == MC_PTR) {
    sb_printf(out, "static %s uchar* ", space_qual(m, spec->ret_space));
  } else {
    const char *type = storage_type(fn->ret_kind);
    if (!type || fn->ret_cls == MC_F64) {
      check_f64(fn, MC_F64);
      type = "int";
    }
    sb_printf(out, "static %s ", type);
  }
  sb_printf(out, "%s(", spec->name);
  for (size_t p = 0; p < func->parameter_count; p++) {
    MslVar *var = func->parameter_names && func->parameter_names[p]
                      ? find_var(fn, func->parameter_names[p])
                      : NULL;
    if (!first) {
      sb_puts(out, ", ");
    }
    first = 0;
    if (!var) {
      sb_printf(out, "int mtl_unused_%zu", p);
      continue;
    }
    param_decl(fn, var, out);
  }
  builtin_params(spec->builtins, out, &first, 0);
  sb_puts(out, ")");
}

static void emit_body(MslFn *fn) {
  GpuStructure structure;
  memset(&structure, 0, sizeof(structure));
  if (!structure_function(fn, &structure)) {
    return;
  }
  if (structure.uses_exit) {
    line(fn, "int mtl_exit = 0;");
    report_note(fn, NULL, "control", "control flow", IR_DEVICE_FACT,
                "rebuilt with an exit variable tested after each loop or block",
                "a branch leaves more than one enclosing loop or block at "
                "once, and Metal has no goto",
                "structured-exit");
  }
  declare_locals(fn);
  declare_residency(fn);
  emit_seq(fn, structure.root);
  gpu_structure_free(&structure);
}

static void emit_helper(MslMod *m, MslSpec *spec, Sb *out) {
  MslFn *fn = spec->fn;
  Sb signature = {0};
  helper_signature(m, spec, &signature);
  sb_free(&fn->out);
  fn->indent = 1;
  report_function(fn);
  emit_body(fn);
  sb_printf(out, "%s {\n%s}\n\n", sb_str(&signature), sb_str(&fn->out));
  sb_free(&signature);
}

static const char *args_field_type(MslMod *m, const MtlcType *type,
                                   const char *type_name, char *buffer,
                                   size_t size) {
  MtlcTypeKind kind = type_kind(type, type_name);
  if (type_is_record(type)) {
    return record_name(m, type);
  }
  if (class_of_kind(kind) == MC_PTR) {
    MtlcTypeKind elem = pointee_kind(type, type_name);
    const char *base = elem == MTLC_TYPE_POINTER || elem == MTLC_TYPE_VOID
                           ? "uchar"
                           : storage_type(elem);
    int read_only = type && type->kind == MTLC_TYPE_POINTER &&
                    type->address_space == MTLC_ADDRESS_SPACE_CONSTANT;
    snprintf(buffer, size, "%s%s %s*", read_only ? "const " : "",
             device_qual(m), base ? base : "uchar");
    return buffer;
  }
  return storage_type(kind);
}

static void emit_kernel(MslMod *m, MslSpec *spec, Sb *out) {
  MslFn *fn = spec->fn;
  IRFunction *func = fn->func;
  Sb signature = {0};
  Sb args = {0};
  int first = 1;
  int has_args = func->parameter_count > 0;
  long long threads = 0;
  if (!fn->returns_void) {
    mod_error(m, "Metal: kernel '%s' must return nothing", fn_name(fn));
    return;
  }
  if (has_args) {
    sb_printf(&args, "struct mtl_args_%s {\n", spec->name);
    for (size_t p = 0; p < func->parameter_count; p++) {
      char buffer[256];
      const char *type = args_field_type(m, param_type(fn, p),
                                         param_type_name(fn, p), buffer,
                                         sizeof(buffer));
      MslVar *var = func->parameter_names && func->parameter_names[p]
                        ? find_var(fn, func->parameter_names[p])
                        : NULL;
      if (!type || (var && var->cls == MC_F64)) {
        mod_error(m,
                  "Metal: kernel '%s' parameter %zu has a type Metal "
                  "cannot hold%s",
                  fn_name(fn), p,
                  var && var->cls == MC_F64 ? " (float64 has no Metal form)"
                                            : "");
        sb_free(&args);
        return;
      }
      sb_printf(&args, "  %s p%zu;\n", type, p);
    }
    sb_puts(&args, "};\n\n");
  }
  if (func->kernel_block[0] > 0) {
    threads = (long long)func->kernel_block[0] *
              (func->kernel_block[1] > 0 ? func->kernel_block[1] : 1) *
              (func->kernel_block[2] > 0 ? func->kernel_block[2] : 1);
  }
  if (threads > 0) {
    sb_printf(&signature, "[[max_total_threads_per_threadgroup(%lld)]]\n",
              threads);
  }
  sb_printf(&signature, "kernel void %s(", spec->name);
  if (has_args) {
    sb_printf(&signature, "constant mtl_args_%s& mtl_args [[buffer(0)]]",
              spec->name);
    first = 0;
  }
  if (fn->has_dynamic) {
    if (!first) {
      sb_puts(&signature, ",\n    ");
    }
    sb_puts(&signature, "threadgroup uchar* mtl_arena [[threadgroup(0)]]");
    first = 0;
  }
  if (!first && spec->builtins) {
    sb_puts(&signature, ",\n    ");
  }
  {
    int bfirst = 1;
    builtin_params(spec->builtins, &signature, &bfirst, 1);
  }
  sb_puts(&signature, ")");
  sb_free(&fn->out);
  fn->indent = 1;
  report_function(fn);
  for (size_t p = 0; p < func->parameter_count && !m->error; p++) {
    MslVar *var = func->parameter_names && func->parameter_names[p]
                      ? find_var(fn, func->parameter_names[p])
                      : NULL;
    if (!var) {
      continue;
    }
    if (var->cls == MC_PTR) {
      line(fn, "%s uchar* %s = (%s uchar*)mtl_args.p%zu;", device_qual(m),
           var->ident, device_qual(m), p);
    } else if (var->cls == MC_REC) {
      line(fn, "%s %s = mtl_args.p%zu;", record_name(m, var->rec), var->ident,
           p);
    } else {
      const char *type = var->storage != MTLC_TYPE_VOID
                             ? storage_type(var->storage)
                             : class_type(var->cls);
      line(fn, "%s %s = mtl_args.p%zu;", type ? type : "int", var->ident, p);
    }
  }
  emit_body(fn);
  sb_puts(out, sb_str(&args));
  sb_printf(out, "%s {\n%s}\n\n", sb_str(&signature), sb_str(&fn->out));
  sb_free(&signature);
  sb_free(&args);
}

static void emit_prelude(MslMod *m, Sb *out) {
  if (m->needs & NEED_DP4A_U) {
    sb_puts(out,
            "static uint mtl_dp4a_u32(uint a, uint b, uint c) {\n"
            "  for (uint i = 0u; i < 4u; i++) {\n"
            "    c += ((a >> (8u * i)) & 0xffu) * ((b >> (8u * i)) & 0xffu);\n"
            "  }\n"
            "  return c;\n"
            "}\n\n");
  }
  if (m->needs & NEED_DP4A_S) {
    sb_puts(out,
            "static int mtl_dp4a_s32(int a, int b, int c) {\n"
            "  uint acc = as_type<uint>(c);\n"
            "  for (uint i = 0u; i < 4u; i++) {\n"
            "    int x = (int)(char)(uchar)((as_type<uint>(a) >> (8u * i)) & "
            "0xffu);\n"
            "    int y = (int)(char)(uchar)((as_type<uint>(b) >> (8u * i)) & "
            "0xffu);\n"
            "    acc += as_type<uint>(x * y);\n"
            "  }\n"
            "  return as_type<int>(acc);\n"
            "}\n\n");
  }
  if (m->needs & NEED_DP2A_U) {
    sb_puts(out,
            "static uint mtl_dp2a_u32(uint a, uint b, uint c, uint hi) {\n"
            "  uint base = hi != 0u ? 16u : 0u;\n"
            "  c += (a & 0xffffu) * ((b >> base) & 0xffu);\n"
            "  c += (a >> 16) * ((b >> (base + 8u)) & 0xffu);\n"
            "  return c;\n"
            "}\n\n");
  }
  if (m->needs & NEED_DP2A_S) {
    sb_puts(out,
            "static int mtl_dp2a_s32(int a, int b, int c, uint hi) {\n"
            "  uint ua = as_type<uint>(a);\n"
            "  uint ub = as_type<uint>(b);\n"
            "  uint base = hi != 0u ? 16u : 0u;\n"
            "  int a0 = (int)(short)(ushort)(ua & 0xffffu);\n"
            "  int a1 = (int)(short)(ushort)(ua >> 16);\n"
            "  int b0 = (int)(char)(uchar)((ub >> base) & 0xffu);\n"
            "  int b1 = (int)(char)(uchar)((ub >> (base + 8u)) & 0xffu);\n"
            "  uint acc = as_type<uint>(c) + as_type<uint>(a0 * b0) + "
            "as_type<uint>(a1 * b1);\n"
            "  return as_type<int>(acc);\n"
            "}\n\n");
  }
  if (m->needs & NEED_PRMT) {
    sb_puts(out,
            "static uint mtl_prmt(uint a, uint b, uint s) {\n"
            "  uint r = 0u;\n"
            "  for (uint i = 0u; i < 4u; i++) {\n"
            "    uint sel = (s >> (4u * i)) & 0xfu;\n"
            "    uint idx = sel & 7u;\n"
            "    uint src = idx < 4u ? a : b;\n"
            "    uint v = (src >> (8u * (idx & 3u))) & 0xffu;\n"
            "    if ((sel & 8u) != 0u) {\n"
            "      v = (v & 0x80u) != 0u ? 0xffu : 0u;\n"
            "    }\n"
            "    r |= v << (8u * i);\n"
            "  }\n"
            "  return r;\n"
            "}\n\n");
  }
}

static int function_orders_device_memory(const IRFunction *func) {
  for (size_t i = 0; i < func->instruction_count; i++) {
    const IRInstruction *in = &func->instructions[i];
    if (in->op == IR_OP_CALL && ir_intrinsic_is_atomic(in->intrinsic) &&
        in->memory_order != MTLC_MEMORY_ORDER_RELAXED &&
        in->address_space != MTLC_ADDRESS_SPACE_WORKGROUP &&
        (in->memory_scope == MTLC_MEMORY_SCOPE_DEVICE ||
         in->memory_scope == MTLC_MEMORY_SCOPE_SYSTEM ||
         in->memory_scope == MTLC_MEMORY_SCOPE_DEFAULT)) {
      return 1;
    }
  }
  return 0;
}

static unsigned char *coherence_by_function(MslMod *m) {
  size_t count = m->program->function_count;
  unsigned char *needs = calloc(count ? count : 1, 1);
  int changed = 1;
  if (!needs) {
    mod_error(m, "Metal: out of memory");
    return NULL;
  }
  for (size_t f = 0; f < count; f++) {
    IRFunction *func = m->program->functions[f];
    needs[f] = func && m->graph.reachable && m->graph.reachable[f] &&
               function_orders_device_memory(func);
  }
  while (changed) {
    changed = 0;
    for (size_t f = 0; f < count; f++) {
      IRFunction *func = m->program->functions[f];
      if (!func || needs[f] || !m->graph.reachable || !m->graph.reachable[f]) {
        continue;
      }
      for (size_t i = 0; i < func->instruction_count; i++) {
        const IRInstruction *in = &func->instructions[i];
        size_t callee = 0;
        if (in->op == IR_OP_CALL && in->intrinsic == MTLC_INTRINSIC_NONE &&
            lookup_function(m, in->text, &callee) && callee < count &&
            needs[callee]) {
          needs[f] = 1;
          changed = 1;
          break;
        }
      }
    }
  }
  return needs;
}

void msl_emit_default_options(MslEmitOptions *options) {
  if (!options) {
    return;
  }
  options->version_major = 3;
  options->version_minor = 2;
  options->fast_math = 0;
  options->gpu_checks = 0;
}

int msl_parse_version(const char *text, MslEmitOptions *options) {
  static const struct {
    const char *text;
    int major;
    int minor;
  } versions[] = {{"3.1", 3, 1}, {"3.2", 3, 2}, {"4.0", 4, 0}, {"4", 4, 0},
                  {"4.1", 4, 1}};
  if (!text || !options) {
    return 0;
  }
  for (size_t i = 0; i < sizeof(versions) / sizeof(versions[0]); i++) {
    if (!strcmp(text, versions[i].text)) {
      options->version_major = versions[i].major;
      options->version_minor = versions[i].minor;
      return 1;
    }
  }
  return 0;
}

static void free_module(MslMod *m) {
  for (size_t s = 0; s < m->nspecs; s++) {
    fn_free(m->specs[s].fn);
    free(m->specs[s].param_space);
    free(m->specs[s].call_spec);
  }
  free(m->specs);
  free(m->records);
  sb_free(&m->records_out);
  ir_gpu_call_graph_destroy(&m->graph);
}

static void report_begin(const MslMod *m) {
  char profile[32];
  snprintf(profile, sizeof(profile), "%d.%d", m->options.version_major,
           m->options.version_minor);
  ir_explain_device_begin("metal", profile);
}

static void report_specializations(const MslMod *m) {
  for (size_t s = 0; s < m->nspecs && !m->error; s++) {
    const IRFunction *func = m->specs[s].fn->func;
    size_t copies = 0;
    size_t first = s;
    char headline[120];
    for (size_t o = 0; o < m->nspecs; o++) {
      if (m->specs[o].function_index == m->specs[s].function_index) {
        copies++;
        first = o < first ? o : first;
      }
    }
    if (copies < 2 || first != s) {
      continue;
    }
    snprintf(headline, sizeof(headline),
             "compiled %zu times, once for each address space it is called "
             "with",
             copies);
    ir_explain_device_note(func->name, func->location.line, "specialization",
                           "helper", IR_DEVICE_FACT, headline,
                           "Metal has no generic address space", NULL,
                           "helper-specialized");
  }
}

int msl_emit_program(IRProgram *program, FILE *out,
                     const MslEmitOptions *options, char **error) {
  MslMod m;
  char *graph_error = NULL;
  Sb helpers = {0};
  Sb kernels = {0};
  Sb protos = {0};
  Sb prelude = {0};
  unsigned char *coherence = NULL;
  int ok;
  if (error) {
    *error = NULL;
  }
  if (!program || !out) {
    if (error) {
      *error = strdup("Metal: no program to emit");
    }
    return 0;
  }
  memset(&m, 0, sizeof(m));
  m.program = program;
  if (options) {
    m.options = *options;
  } else {
    msl_emit_default_options(&m.options);
  }
  if (!ir_program_build_gpu_call_graph(program, &m.graph, &graph_error)) {
    if (error) {
      *error = graph_error ? graph_error : strdup("Metal: invalid GPU call graph");
    } else {
      free(graph_error);
    }
    return 0;
  }
  coherence = coherence_by_function(&m);
  for (size_t f = 0; f < program->function_count && coherence && !m.error;
       f++) {
    IRFunction *func = program->functions[f];
    if (func && func->is_kernel && m.graph.reachable && m.graph.reachable[f]) {
      MslSpace *spaces = calloc(func->parameter_count + 1, sizeof(MslSpace));
      if (!spaces) {
        mod_error(&m, "Metal: out of memory");
        break;
      }
      if (coherence[f] && !version_at_least(&m, 3, 2)) {
        mod_error(&m,
                  "Metal: '%s' orders device memory with atomics, which "
                  "needs coherent device memory; that starts at "
                  "--metal-version=3.2",
                  func->name);
      }
      find_or_add_spec(&m, f, spaces, func->parameter_count, coherence[f]);
      free(spaces);
    }
  }
  free(coherence);
  if (!m.error) {
    report_begin(&m);
    analyze_all(&m);
  }
  for (size_t s = 0; s < m.nspecs && !m.error; s++) {
    if (m.specs[s].fn) {
      m.specs[s].fn->spec = &m.specs[s];
    }
  }
  if (!m.error) {
    compute_builtins(&m);
  }
  for (size_t s = 0; s < m.nspecs && !m.error; s++) {
    MslSpec *spec = &m.specs[s];
    m.coherent_device = spec->coherent;
    if (spec->fn->func->is_kernel) {
      emit_kernel(&m, spec, &kernels);
    } else {
      Sb proto = {0};
      helper_signature(&m, spec, &proto);
      sb_printf(&protos, "%s;\n", sb_str(&proto));
      sb_free(&proto);
      emit_helper(&m, spec, &helpers);
    }
  }
  report_specializations(&m);
  if (!m.error) {
    emit_prelude(&m, &prelude);
    fprintf(out, "#include <metal_stdlib>\n");
    fprintf(out, "using namespace metal;\n\n");
    if (version_at_least(&m, 3, 2)) {
      fprintf(out, "#pragma METAL fp math_mode(%s)\n",
              m.options.fast_math ? "fast" : "safe");
      fprintf(out, "#pragma METAL fp contract(%s)\n\n",
              m.options.fast_math ? "fast" : "off");
    }
    fputs(sb_str(&m.records_out), out);
    fputs(sb_str(&prelude), out);
    if (protos.len) {
      fputs(sb_str(&protos), out);
      fputs("\n", out);
    }
    fputs(sb_str(&helpers), out);
    fputs(sb_str(&kernels), out);
  }
  ok = m.error == NULL;
  if (!ok && error) {
    *error = m.error;
    m.error = NULL;
  }
  free(m.error);
  sb_free(&helpers);
  sb_free(&kernels);
  sb_free(&protos);
  sb_free(&prelude);
  free_module(&m);
  return ok;
}
