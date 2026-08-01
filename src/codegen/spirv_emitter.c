/* IR -> SPIR-V binary emitter (OpenCL 2.0 environment). See spirv_emitter.h.
 *
 * SPIR-V sibling of ptx_emitter.c. Two things make SPIR-V different from PTX:
 *
 *  1. It is a *binary* word stream (little-endian 32-bit words), laid out in a
 *     fixed section order (capabilities, ext-imports, memory model, entry
 *     points, decorations, types+constants+globals, then function bodies). We
 *     buffer each section separately and concatenate at the end, so emission
 *     order need not match layout order.
 *
 *  2. Control flow maps DIRECTLY: the IR's label/branch stream is cut into
 *     basic blocks and each becomes one SPIR-V block -- IR_OP_JUMP is OpBranch,
 *     IR_OP_BRANCH_ZERO/_EQ are OpBranchConditional, IR_OP_RETURN is OpReturn.
 *     SPIR-V's structured-control-flow rules (OpSelectionMerge/OpLoopMerge)
 *     are mandated only by the Shader capability; Kernel (OpenCL) modules may
 *     branch freely, exactly like PTX `bra` -- spirv-val --target-env opencl2.0
 *     accepts arbitrary unstructured branches and back-edges. Every IR value
 *     lives in a Function-storage variable (reg2mem) so there are never
 *     cross-block SSA references (no OpPhi); a driver's SPIR-V consumer
 *     promotes them back to registers.
 *
 * Pointers follow the PTX model: a kernel pointer parameter keeps its neutral
 * global/workgroup address space, is immediately OpConvertPtrToU'd to a 64-bit
 * integer that all arithmetic runs on, then OpConvertUToPtr'd back to the same
 * typed storage-class pointer at each access. Legacy/generic kernel pointers
 * use CrossWorkgroup in the current OpenCL 2.0 ABI. This needs the Addresses capability
 * and matches the IR, whose address arithmetic is already integer ops. */
#include "spirv_emitter.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---- SPIR-V constants (from the core spec / OpenCL env) ---- */
#define SPV_MAGIC 0x07230203u
#define SPV_VERSION_1_0 0x00010000u

/* opcodes */
enum {
  Op_Extension = 10,
  Op_ExtInstImport = 11,
  Op_ExtInst = 12,
  Op_MemoryModel = 14,
  Op_EntryPoint = 15,
  Op_ExecutionMode = 16,
  Op_Capability = 17,
  Op_TypeVoid = 19,
  Op_TypeBool = 20,
  Op_TypeInt = 21,
  Op_TypeFloat = 22,
  Op_TypeVector = 23,
  Op_TypeArray = 28,
  Op_TypePointer = 32,
  Op_TypeFunction = 33,
  Op_Constant = 43,
  Op_Function = 54,
  Op_FunctionParameter = 55,
  Op_FunctionEnd = 56,
  Op_FunctionCall = 57,
  Op_Variable = 59,
  Op_Load = 61,
  Op_Store = 62,
  Op_VectorExtractDynamic = 77,
  Op_CompositeExtract = 81,
  Op_ConvertFToU = 109,
  Op_ConvertFToS = 110,
  Op_ConvertSToF = 111,
  Op_ConvertUToF = 112,
  Op_UConvert = 113,
  Op_SConvert = 114,
  Op_FConvert = 115,
  Op_ConvertPtrToU = 117,
  Op_ConvertUToPtr = 120,
  Op_Bitcast = 124,
  Op_SNegate = 126,
  Op_FNegate = 127,
  Op_IAdd = 128,
  Op_FAdd = 129,
  Op_ISub = 130,
  Op_FSub = 131,
  Op_IMul = 132,
  Op_FMul = 133,
  Op_UDiv = 134,
  Op_SDiv = 135,
  Op_FDiv = 136,
  Op_UMod = 137,
  Op_SRem = 138,
  Op_FRem = 140,
  Op_ShiftRightLogical = 194,
  Op_ShiftRightArithmetic = 195,
  Op_ShiftLeftLogical = 196,
  Op_BitwiseOr = 197,
  Op_BitwiseXor = 198,
  Op_BitwiseAnd = 199,
  Op_Not = 200,
  Op_Select = 169,
  Op_IEqual = 170,
  Op_INotEqual = 171,
  Op_UGreaterThan = 172,
  Op_SGreaterThan = 173,
  Op_UGreaterThanEqual = 174,
  Op_SGreaterThanEqual = 175,
  Op_ULessThan = 176,
  Op_SLessThan = 177,
  Op_ULessThanEqual = 178,
  Op_SLessThanEqual = 179,
  Op_FOrdEqual = 180,
  Op_FUnordNotEqual = 183,
  Op_FOrdLessThan = 184,
  Op_FOrdGreaterThan = 186,
  Op_FOrdLessThanEqual = 188,
  Op_FOrdGreaterThanEqual = 190,
  Op_ControlBarrier = 224,
  Op_AtomicLoad = 227,
  Op_AtomicStore = 228,
  Op_AtomicExchange = 229,
  Op_AtomicCompareExchange = 230,
  Op_AtomicIAdd = 234,
  Op_AtomicISub = 235,
  Op_AtomicUMin = 237,
  Op_AtomicUMax = 239,
  Op_AtomicAnd = 240,
  Op_AtomicOr = 241,
  Op_AtomicXor = 242,
  Op_GroupBroadcast = 263,
  Op_GroupIAdd = 264,
  Op_GroupFAdd = 265,
  Op_GroupFMin = 266,
  Op_GroupUMin = 267,
  Op_GroupFMax = 269,
  Op_GroupUMax = 270,
  Op_SubgroupBallotKHR = 4421,
  Op_SubgroupAllKHR = 4428,
  Op_SubgroupAnyKHR = 4429,
  Op_Decorate = 71,
  Op_Label = 248,
  Op_Branch = 249,
  Op_BranchConditional = 250,
  Op_Return = 253,
  Op_ReturnValue = 254
};

/* capabilities */
enum {
  Cap_Addresses = 4,
  Cap_Kernel = 6,
  Cap_Float16 = 9,
  Cap_Float64 = 10,
  Cap_Int64 = 11,
  Cap_Int64Atomics = 12,
  Cap_Groups = 18,
  Cap_Int16 = 22,
  Cap_Int8 = 39,
  Cap_SubgroupBallotKHR = 4423,
  Cap_SubgroupVoteKHR = 4431
};

/* storage classes */
enum {
  SC_UniformConstant = 0,
  SC_Input = 1,
  SC_Workgroup = 4,
  SC_CrossWorkgroup = 5,
  SC_Private = 6,
  SC_Function = 7
};

/* misc enums */
enum { AddrModel_Physical64 = 2, MemModel_OpenCL = 2, ExecModel_Kernel = 6 };
enum { Decoration_BuiltIn = 11 };
enum { MemAccess_Aligned = 2 };
enum {
  Scope_CrossDevice = 0,
  Scope_Device = 1,
  Scope_Workgroup = 2,
  Scope_Subgroup = 3,
  Scope_Invocation = 4
};
/* SequentiallyConsistent(0x10) | WorkgroupMemory(0x100) */
enum {
  Sem_None = 0,
  Sem_Acquire = 0x2,
  Sem_Release = 0x4,
  Sem_AcquireRelease = 0x8,
  Sem_SequentiallyConsistent = 0x10,
  Sem_UniformMemory = 0x40,
  Sem_WorkgroupMemory = 0x100,
  Sem_CrossWorkgroupMemory = 0x200,
  Sem_WorkgroupBarrier = 0x110
};

/* BuiltIn ids */
enum {
  BI_NumWorkgroups = 24,
  BI_WorkgroupSize = 25,
  BI_WorkgroupId = 26,
  BI_LocalInvocationId = 27,
  BI_SubgroupSize = 36,
  BI_SubgroupLocalInvocationId = 41
};

/* OpenCL.std extended instruction numbers */
enum {
  CL_cos = 14,
  CL_exp = 19,
  CL_fabs = 23,
  CL_log = 37,
  CL_rsqrt = 56,
  CL_sin = 57,
  CL_sqrt = 61
};

/* ---- growable word buffer ---- */
typedef struct {
  uint32_t *data;
  size_t len, cap;
} Wb;

static void wb_push(Wb *w, uint32_t v) {
  if (w->len == w->cap) {
    w->cap = w->cap ? w->cap * 2 : 64;
    w->data = realloc(w->data, w->cap * sizeof(uint32_t));
  }
  w->data[w->len++] = v;
}
static void wb_free(Wb *w) {
  free(w->data);
  w->data = NULL;
  w->len = w->cap = 0;
}
static void wb_str(Wb *w, const char *s) {
  size_t len = strlen(s);
  size_t nwords = len / 4 + 1; /* room for the terminating NUL + zero pad */
  for (size_t i = 0; i < nwords; i++) {
    uint32_t word = 0;
    for (int b = 0; b < 4; b++) {
      size_t idx = i * 4 + (size_t)b;
      unsigned char c = idx < len ? (unsigned char)s[idx] : 0;
      word |= (uint32_t)c << (8 * b);
    }
    wb_push(w, word);
  }
}
/* emit an instruction whose operands are already in `ops` */
static void emit_ops(Wb *sec, uint16_t opcode, const Wb *ops) {
  wb_push(sec, ((uint32_t)(ops->len + 1) << 16) | opcode);
  for (size_t i = 0; i < ops->len; i++) {
    wb_push(sec, ops->data[i]);
  }
}
/* emit an instruction with up to 16 fixed operands passed as unsigned ints */
static void emitv(Wb *sec, uint16_t opcode, size_t nops, ...) {
  uint32_t tmp[16];
  va_list ap;
  va_start(ap, nops);
  for (size_t i = 0; i < nops && i < 16; i++) {
    tmp[i] = va_arg(ap, unsigned);
  }
  va_end(ap);
  wb_push(sec, ((uint32_t)(nops + 1) << 16) | opcode);
  for (size_t i = 0; i < nops; i++) {
    wb_push(sec, tmp[i]);
  }
}

/* ---- interning cache ---- */
typedef struct {
  char *key;
  uint32_t id;
} CacheEnt;

typedef struct {
  Wb caps, extimports, memmodel, entrypoints, execmodes, decorations,
      typesconsts, functions;
  uint32_t next_id;
  CacheEnt *cache; /* types + constants + pointer types, keyed by string */
  size_t ncache, capcache;
  uint32_t opencl_ext;
  uint32_t builtin_var[64]; /* BuiltIn id -> Input variable id (0 = none yet) */
  int use_int8, use_int16, use_float16, use_float64, use_atomics64;
  int use_subgroups, use_subgroup_ballot, use_subgroup_vote;
  IRProgram *program;
  uint32_t *device_function_ids; /* indexed like program->functions */
  uint64_t *function_builtin_masks;
  char *error;
} SpvMod;

static uint32_t new_id(SpvMod *m) { return m->next_id++; }

static void mod_error(SpvMod *m, const char *fmt, ...) {
  if (m->error) {
    return;
  }
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  m->error = strdup(buf);
}

static uint32_t cache_get(SpvMod *m, const char *key) {
  for (size_t i = 0; i < m->ncache; i++) {
    if (strcmp(m->cache[i].key, key) == 0) {
      return m->cache[i].id;
    }
  }
  return 0;
}
static void cache_put(SpvMod *m, const char *key, uint32_t id) {
  if (m->ncache == m->capcache) {
    m->capcache = m->capcache ? m->capcache * 2 : 32;
    m->cache = realloc(m->cache, m->capcache * sizeof(CacheEnt));
  }
  m->cache[m->ncache].key = strdup(key);
  m->cache[m->ncache].id = id;
  m->ncache++;
}

/* ---- types ---- */
static uint32_t type_void(SpvMod *m) {
  uint32_t id = cache_get(m, "void");
  if (id) return id;
  id = new_id(m);
  emitv(&m->typesconsts, Op_TypeVoid, 1, id);
  cache_put(m, "void", id);
  return id;
}
static uint32_t type_int(SpvMod *m, int width) {
  char key[16];
  snprintf(key, sizeof(key), "i%d", width);
  uint32_t id = cache_get(m, key);
  if (id) return id;
  if (width == 8) m->use_int8 = 1;
  if (width == 16) m->use_int16 = 1;
  id = new_id(m);
  /* OpenCL environment: OpTypeInt signedness must be 0 (signed vs unsigned is
   * expressed by the operations, e.g. OpSDiv vs OpUDiv). */
  emitv(&m->typesconsts, Op_TypeInt, 3, id, (unsigned)width, 0u);
  cache_put(m, key, id);
  return id;
}
static uint32_t type_float(SpvMod *m, int width) {
  char key[16];
  snprintf(key, sizeof(key), "f%d", width);
  uint32_t id = cache_get(m, key);
  if (id) return id;
  if (width == 16) m->use_float16 = 1;
  if (width == 64) m->use_float64 = 1;
  id = new_id(m);
  emitv(&m->typesconsts, Op_TypeFloat, 2, id, (unsigned)width);
  cache_put(m, key, id);
  return id;
}
static uint32_t type_bool(SpvMod *m) {
  uint32_t id = cache_get(m, "bool");
  if (id) return id;
  id = new_id(m);
  emitv(&m->typesconsts, Op_TypeBool, 1, id);
  cache_put(m, "bool", id);
  return id;
}
static uint32_t type_pointer(SpvMod *m, int sc, uint32_t pointee) {
  char key[48];
  snprintf(key, sizeof(key), "p%d:%u", sc, pointee);
  uint32_t id = cache_get(m, key);
  if (id) return id;
  id = new_id(m);
  emitv(&m->typesconsts, Op_TypePointer, 3, id, (unsigned)sc, pointee);
  cache_put(m, key, id);
  return id;
}
static uint32_t type_vec3_ulong(SpvMod *m) {
  uint32_t id = cache_get(m, "v3ulong");
  if (id) return id;
  uint32_t u64 = type_int(m, 64);
  id = new_id(m);
  emitv(&m->typesconsts, Op_TypeVector, 3, id, u64, 3u);
  cache_put(m, "v3ulong", id);
  return id;
}
static uint32_t type_vec4_uint(SpvMod *m) {
  uint32_t id = cache_get(m, "v4uint");
  if (id) return id;
  uint32_t u32 = type_int(m, 32);
  id = new_id(m);
  emitv(&m->typesconsts, Op_TypeVector, 3, id, u32, 4u);
  cache_put(m, "v4uint", id);
  return id;
}

/* map an MtlcTypeKind to (width, is-float) */
static int kind_is_float(MtlcTypeKind k) {
  return k == MTLC_TYPE_FLOAT32 || k == MTLC_TYPE_FLOAT64;
}
static int kind_is_unsigned(MtlcTypeKind k) {
  switch (k) {
  case MTLC_TYPE_UINT8:
  case MTLC_TYPE_UINT16:
  case MTLC_TYPE_UINT32:
  case MTLC_TYPE_UINT64:
  case MTLC_TYPE_BOOL:
  case MTLC_TYPE_POINTER:
  case MTLC_TYPE_ARRAY:
  case MTLC_TYPE_STRING:
  case MTLC_TYPE_FUNCTION_POINTER:
    return 1;
  default:
    return 0;
  }
}
static int kind_bits(MtlcTypeKind k) {
  switch (k) {
  case MTLC_TYPE_INT8:
  case MTLC_TYPE_UINT8:
  case MTLC_TYPE_BOOL:
    return 8;
  case MTLC_TYPE_INT16:
  case MTLC_TYPE_UINT16:
    return 16;
  case MTLC_TYPE_INT32:
  case MTLC_TYPE_UINT32:
  case MTLC_TYPE_FLOAT32:
    return 32;
  default:
    return 64; /* int64/uint64/float64/pointer/array/string/funcptr */
  }
}
static uint32_t kind_type(SpvMod *m, MtlcTypeKind k) {
  switch (k) {
  case MTLC_TYPE_FLOAT32:
    return type_float(m, 32);
  case MTLC_TYPE_FLOAT64:
    return type_float(m, 64);
  case MTLC_TYPE_INT8:
  case MTLC_TYPE_UINT8:
  case MTLC_TYPE_BOOL:
    return type_int(m, 8);
  case MTLC_TYPE_INT16:
  case MTLC_TYPE_UINT16:
    return type_int(m, 16);
  case MTLC_TYPE_INT32:
  case MTLC_TYPE_UINT32:
    return type_int(m, 32);
  default:
    return type_int(m, 64);
  }
}

/* ---- constants ---- */
static uint32_t const_u32(SpvMod *m, uint32_t v) {
  char key[32];
  snprintf(key, sizeof(key), "cu32:%u", v);
  uint32_t id = cache_get(m, key);
  if (id) return id;
  uint32_t t = type_int(m, 32);
  id = new_id(m);
  emitv(&m->typesconsts, Op_Constant, 3, t, id, v);
  cache_put(m, key, id);
  return id;
}
static uint32_t type_array(SpvMod *m, uint32_t element_type, uint32_t count) {
  char key[48];
  snprintf(key, sizeof(key), "a:%u:%u", element_type, count);
  uint32_t id = cache_get(m, key);
  if (id) return id;
  uint32_t length = const_u32(m, count);
  id = new_id(m);
  emitv(&m->typesconsts, Op_TypeArray, 3, id, element_type, length);
  cache_put(m, key, id);
  return id;
}
static uint32_t const_scalar_int(SpvMod *m, MtlcTypeKind k, long long v) {
  int bits = kind_bits(k);
  char key[48];
  snprintf(key, sizeof(key), "ci:%d:%lld", bits, v);
  uint32_t id = cache_get(m, key);
  if (id) return id;
  uint32_t t = kind_type(m, k);
  id = new_id(m);
  if (bits <= 32) {
    uint32_t literal = (uint32_t)v;
    /* OpenCL integer types use Signedness=0 even for language-signed values.
     * SPIR-V therefore requires unused high bits of 8/16-bit literals to be
     * zero; arithmetic instructions supply signed interpretation later. */
    if (bits < 32) literal &= (1u << bits) - 1u;
    emitv(&m->typesconsts, Op_Constant, 3, t, id, (unsigned)literal);
  } else {
    uint64_t u = (uint64_t)v;
    emitv(&m->typesconsts, Op_Constant, 4, t, id, (unsigned)(uint32_t)u,
          (unsigned)(uint32_t)(u >> 32));
  }
  cache_put(m, key, id);
  return id;
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
static uint32_t const_float(SpvMod *m, MtlcTypeKind k, double v) {
  char key[48];
  if (k == MTLC_TYPE_FLOAT32) {
    uint32_t bits = f32_bits(v);
    snprintf(key, sizeof(key), "cf32:%u", bits);
    uint32_t id = cache_get(m, key);
    if (id) return id;
    uint32_t t = type_float(m, 32);
    id = new_id(m);
    emitv(&m->typesconsts, Op_Constant, 3, t, id, bits);
    cache_put(m, key, id);
    return id;
  } else {
    uint64_t bits = f64_bits(v);
    snprintf(key, sizeof(key), "cf64:%llu", (unsigned long long)bits);
    uint32_t id = cache_get(m, key);
    if (id) return id;
    uint32_t t = type_float(m, 64);
    id = new_id(m);
    emitv(&m->typesconsts, Op_Constant, 4, t, id, (unsigned)(uint32_t)bits,
          (unsigned)(uint32_t)(bits >> 32));
    cache_put(m, key, id);
    return id;
  }
}

/* ---- BuiltIn Input variables ---- */
static int builtin_is_subgroup_scalar(int builtin) {
  return builtin == BI_SubgroupSize ||
         builtin == BI_SubgroupLocalInvocationId;
}

static uint32_t builtin_var(SpvMod *m, int builtin) {
  if (m->builtin_var[builtin]) {
    return m->builtin_var[builtin];
  }
  uint32_t value_type = builtin_is_subgroup_scalar(builtin)
                            ? type_int(m, 32)
                            : type_vec3_ulong(m);
  uint32_t pt = type_pointer(m, SC_Input, value_type);
  uint32_t id = new_id(m);
  emitv(&m->typesconsts, Op_Variable, 3, pt, id, (unsigned)SC_Input);
  emitv(&m->decorations, Op_Decorate, 3, id, (unsigned)Decoration_BuiltIn,
        (unsigned)builtin);
  m->builtin_var[builtin] = id;
  return id;
}

/* ============================ value model ============================ */
/* A value descriptor, mirroring the PTX PtxVal (class/ptr/elem). Pointers are
 * carried as 64-bit integers; is_ptr/elem remember the pointee for load/store. */
typedef struct {
  MtlcTypeKind kind;
  int is_unsigned;
  int is_ptr;
  MtlcTypeKind elem;
  MtlcAddressSpace address_space;
} SpvDesc;

typedef struct {
  char *name;
  SpvDesc d;
  uint32_t var_id; /* OpVariable (Function ptr to kind_type(d.kind)) */
} SpvBind;

typedef struct {
  SpvMod *m;
  IRFunction *function;
  size_t function_index;
  SpvDesc return_desc;
  int returns_void;
  SpvBind *binds;
  size_t nbinds, capbinds;
  uint32_t used_builtins[16];
  size_t nused_builtins;
  uint64_t builtin_mask;
} SpvFn;

static SpvBind *find_bind(SpvFn *fn, const char *name) {
  if (!name) return NULL;
  for (size_t i = 0; i < fn->nbinds; i++) {
    if (strcmp(fn->binds[i].name, name) == 0) {
      return &fn->binds[i];
    }
  }
  return NULL;
}
static SpvBind *add_bind(SpvFn *fn, const char *name, SpvDesc d) {
  SpvBind *b = find_bind(fn, name);
  if (b) return b;
  if (fn->nbinds == fn->capbinds) {
    fn->capbinds = fn->capbinds ? fn->capbinds * 2 : 16;
    fn->binds = realloc(fn->binds, fn->capbinds * sizeof(SpvBind));
  }
  b = &fn->binds[fn->nbinds++];
  b->name = strdup(name);
  b->d = d;
  b->var_id = 0;
  return b;
}
static void track_builtin(SpvFn *fn, int builtin, uint32_t var) {
  for (size_t i = 0; i < fn->nused_builtins; i++) {
    if (fn->used_builtins[i] == var) return;
  }
  if (fn->nused_builtins < 16) {
    fn->used_builtins[fn->nused_builtins++] = var;
  }
  if (builtin >= 0 && builtin < 64) {
    fn->builtin_mask |= UINT64_C(1) << builtin;
  }
}

/* ---- type-name parsing (no symbol table), same rules as the PTX backend ---- */
static MtlcTypeKind base_kind_from_name(const char *s, int *is_unsigned) {
  *is_unsigned = (strstr(s, "uint") != NULL || strstr(s, "bool") != NULL);
  if (strstr(s, "float32") || strstr(s, "f32")) return MTLC_TYPE_FLOAT32;
  if (strstr(s, "float64") || strstr(s, "double") || strstr(s, "float"))
    return MTLC_TYPE_FLOAT64;
  if (strstr(s, "int64") || strstr(s, "uint64"))
    return *is_unsigned ? MTLC_TYPE_UINT64 : MTLC_TYPE_INT64;
  if (strstr(s, "int16") || strstr(s, "uint16"))
    return *is_unsigned ? MTLC_TYPE_UINT16 : MTLC_TYPE_INT16;
  if (strstr(s, "int8") || strstr(s, "uint8"))
    return *is_unsigned ? MTLC_TYPE_UINT8 : MTLC_TYPE_INT8;
  if (strstr(s, "int32") || strstr(s, "uint32"))
    return *is_unsigned ? MTLC_TYPE_UINT32 : MTLC_TYPE_INT32;
  if (strstr(s, "bool")) return MTLC_TYPE_BOOL;
  return MTLC_TYPE_INT32;
}
static SpvDesc desc_from_typename(const char *name) {
  SpvDesc v = {0};
  if (!name) name = "int64";
  int ptr = (strchr(name, '*') != NULL) || strstr(name, "cstring") ||
            strstr(name, "string");
  int isu = 0;
  MtlcTypeKind base = base_kind_from_name(name, &isu);
  if (strstr(name, "cstring")) base = MTLC_TYPE_UINT8;
  if (ptr) {
    v.kind = MTLC_TYPE_POINTER;
    v.is_ptr = 1;
    v.is_unsigned = 1;
    const char *fs = strchr(name, '*');
    v.elem = (fs && strchr(fs + 1, '*')) ? MTLC_TYPE_POINTER : base;
    v.address_space = MTLC_ADDRESS_SPACE_GENERIC;
  } else {
    v.kind = base;
    v.is_unsigned = kind_is_unsigned(base);
    v.elem = MTLC_TYPE_VOID;
  }
  return v;
}

static SpvDesc desc_from_type(const MtlcType *type) {
  SpvDesc v = {0};
  if (!type) return desc_from_typename(NULL);
  if (type->kind == MTLC_TYPE_POINTER) {
    v.kind = MTLC_TYPE_POINTER;
    v.is_ptr = 1;
    v.is_unsigned = 1;
    v.elem = type->base_type ? type->base_type->kind : MTLC_TYPE_VOID;
    v.address_space = type->address_space == MTLC_ADDRESS_SPACE_DEFAULT
                          ? MTLC_ADDRESS_SPACE_GLOBAL
                          : type->address_space;
  } else {
    v.kind = type->kind;
    v.is_unsigned = kind_is_unsigned(type->kind);
    v.elem = MTLC_TYPE_VOID;
  }
  return v;
}

static int spv_storage_class(MtlcAddressSpace address_space) {
  switch (address_space) {
  case MTLC_ADDRESS_SPACE_DEFAULT:
  case MTLC_ADDRESS_SPACE_GENERIC:
  case MTLC_ADDRESS_SPACE_GLOBAL:
    /* The current ABI conservatively represents generic and legacy kernel
     * pointers as global pointers instead of requiring GenericPointer. */
    return SC_CrossWorkgroup;
  case MTLC_ADDRESS_SPACE_WORKGROUP: return SC_Workgroup;
  case MTLC_ADDRESS_SPACE_CONSTANT: return SC_UniformConstant;
  /* Mettle `private` storage is per invocation and has function lifetime.
   * SPIR-V's Private storage class is module scope; using it for a local array
   * is invalid in the OpenCL environment. */
  case MTLC_ADDRESS_SPACE_PRIVATE: return SC_Function;
  }
  return -1;
}

static SpvDesc operand_desc(SpvFn *fn, const IROperand *op) {
  SpvDesc v = {0};
  if (op->kind == IR_OPERAND_INT) {
    v.kind = (op->int_value > 2147483647LL || op->int_value < -2147483648LL)
                 ? MTLC_TYPE_INT64
                 : MTLC_TYPE_INT32;
    return v;
  }
  if (op->kind == IR_OPERAND_FLOAT) {
    v.kind = (op->float_bits == 32) ? MTLC_TYPE_FLOAT32 : MTLC_TYPE_FLOAT64;
    return v;
  }
  SpvBind *b = find_bind(fn, op->name);
  if (b) return b->d;
  v.kind = MTLC_TYPE_INT32;
  return v;
}

/* ---- intrinsics classification (shared by pre-pass and body) ---- */
static int sreg_component(MtlcIntrinsic intrinsic, int *builtin) {
  switch (intrinsic) {
  case MTLC_INTRINSIC_GPU_LOCAL_ID_X: *builtin = BI_LocalInvocationId; return 0;
  case MTLC_INTRINSIC_GPU_LOCAL_ID_Y: *builtin = BI_LocalInvocationId; return 1;
  case MTLC_INTRINSIC_GPU_LOCAL_ID_Z: *builtin = BI_LocalInvocationId; return 2;
  case MTLC_INTRINSIC_GPU_LOCAL_SIZE_X: *builtin = BI_WorkgroupSize; return 0;
  case MTLC_INTRINSIC_GPU_LOCAL_SIZE_Y: *builtin = BI_WorkgroupSize; return 1;
  case MTLC_INTRINSIC_GPU_LOCAL_SIZE_Z: *builtin = BI_WorkgroupSize; return 2;
  case MTLC_INTRINSIC_GPU_GROUP_ID_X: *builtin = BI_WorkgroupId; return 0;
  case MTLC_INTRINSIC_GPU_GROUP_ID_Y: *builtin = BI_WorkgroupId; return 1;
  case MTLC_INTRINSIC_GPU_GROUP_ID_Z: *builtin = BI_WorkgroupId; return 2;
  case MTLC_INTRINSIC_GPU_NUM_GROUPS_X: *builtin = BI_NumWorkgroups; return 0;
  case MTLC_INTRINSIC_GPU_NUM_GROUPS_Y: *builtin = BI_NumWorkgroups; return 1;
  case MTLC_INTRINSIC_GPU_NUM_GROUPS_Z: *builtin = BI_NumWorkgroups; return 2;
  default: return -1;
  }
}
static int is_math_intrinsic(MtlcIntrinsic intrinsic) {
  return intrinsic >= MTLC_INTRINSIC_GPU_SQRT_F32 &&
         intrinsic <= MTLC_INTRINSIC_GPU_EXP_F32;
}
static int is_atomic_intrinsic(MtlcIntrinsic intrinsic) {
  return ir_intrinsic_is_atomic(intrinsic);
}
static int spv_atomic_scope(MtlcMemoryScope scope) {
  switch (scope) {
  case MTLC_MEMORY_SCOPE_WORK_ITEM: return Scope_Invocation;
  case MTLC_MEMORY_SCOPE_SUBGROUP: return Scope_Subgroup;
  case MTLC_MEMORY_SCOPE_WORKGROUP: return Scope_Workgroup;
  case MTLC_MEMORY_SCOPE_DEFAULT:
  case MTLC_MEMORY_SCOPE_DEVICE:
    return Scope_Device;
  case MTLC_MEMORY_SCOPE_SYSTEM: return Scope_CrossDevice;
  }
  return -1;
}

static int spv_atomic_semantics(MtlcMemoryOrder order,
                                MtlcAddressSpace address_space) {
  int ordering;
  int memory;
  switch (order) {
  case MTLC_MEMORY_ORDER_DEFAULT:
  case MTLC_MEMORY_ORDER_RELAXED:
    ordering = Sem_None;
    break;
  case MTLC_MEMORY_ORDER_ACQUIRE: ordering = Sem_Acquire; break;
  case MTLC_MEMORY_ORDER_RELEASE: ordering = Sem_Release; break;
  case MTLC_MEMORY_ORDER_ACQ_REL: ordering = Sem_AcquireRelease; break;
  case MTLC_MEMORY_ORDER_SEQ_CST:
    ordering = Sem_SequentiallyConsistent;
    break;
  default: return -1;
  }
  switch (address_space) {
  case MTLC_ADDRESS_SPACE_DEFAULT:
  case MTLC_ADDRESS_SPACE_GENERIC:
  case MTLC_ADDRESS_SPACE_GLOBAL:
    memory = Sem_CrossWorkgroupMemory;
    break;
  case MTLC_ADDRESS_SPACE_WORKGROUP:
    memory = Sem_WorkgroupMemory;
    break;
  case MTLC_ADDRESS_SPACE_CONSTANT:
    memory = Sem_UniformMemory;
    break;
  case MTLC_ADDRESS_SPACE_PRIVATE:
  default:
    return -1;
  }
  return ordering | memory;
}

static int spv_compare_exchange_failure_valid(MtlcMemoryOrder success,
                                              MtlcMemoryOrder failure) {
  if (failure != MTLC_MEMORY_ORDER_RELAXED &&
      failure != MTLC_MEMORY_ORDER_ACQUIRE &&
      failure != MTLC_MEMORY_ORDER_SEQ_CST)
    return 0;
  if (success == MTLC_MEMORY_ORDER_RELAXED)
    return failure == MTLC_MEMORY_ORDER_RELAXED;
  if (success == MTLC_MEMORY_ORDER_ACQUIRE ||
      success == MTLC_MEMORY_ORDER_ACQ_REL)
    return failure == MTLC_MEMORY_ORDER_RELAXED ||
           failure == MTLC_MEMORY_ORDER_ACQUIRE;
  if (success == MTLC_MEMORY_ORDER_RELEASE)
    return failure == MTLC_MEMORY_ORDER_RELAXED;
  return success == MTLC_MEMORY_ORDER_SEQ_CST;
}

static int spv_barrier_semantics(MtlcMemoryOrder order,
                                 unsigned memory_regions) {
  int semantics;
  switch (order) {
  case MTLC_MEMORY_ORDER_ACQUIRE: semantics = Sem_Acquire; break;
  case MTLC_MEMORY_ORDER_RELEASE: semantics = Sem_Release; break;
  case MTLC_MEMORY_ORDER_ACQ_REL: semantics = Sem_AcquireRelease; break;
  case MTLC_MEMORY_ORDER_DEFAULT:
  case MTLC_MEMORY_ORDER_SEQ_CST:
    semantics = Sem_SequentiallyConsistent;
    break;
  default: return -1;
  }
  if (memory_regions == 0) memory_regions = MTLC_MEMORY_REGION_WORKGROUP;
  if (memory_regions & MTLC_MEMORY_REGION_WORKGROUP)
    semantics |= Sem_WorkgroupMemory;
  if (memory_regions & MTLC_MEMORY_REGION_GLOBAL)
    semantics |= Sem_CrossWorkgroupMemory;
  if (memory_regions & ~(MTLC_MEMORY_REGION_WORKGROUP |
                         MTLC_MEMORY_REGION_GLOBAL))
    return -1;
  return semantics;
}

static void emit_workgroup_barrier(SpvFn *fn, MtlcMemoryOrder order,
                                   unsigned memory_regions) {
  int semantics = spv_barrier_semantics(order, memory_regions);
  if (semantics < 0) {
    mod_error(fn->m, "SPIR-V: invalid workgroup barrier memory contract");
    return;
  }
  uint32_t scope = const_u32(fn->m, Scope_Workgroup);
  uint32_t sem = const_u32(fn->m, (uint32_t)semantics);
  emitv(&fn->m->functions, Op_ControlBarrier, 3, scope, scope, sem);
}

static long spv_function_index(const IRProgram *program, const char *name) {
  if (!program || !name) return -1;
  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *function = program->functions[i];
    if (function && function->name && strcmp(function->name, name) == 0) {
      return (long)i;
    }
  }
  return -1;
}

static const MtlcType *spv_function_return_type(
    const IRProgram *program, const IRFunction *function,
    const IRModuleSymbol *symbol) {
  if (symbol && symbol->kind == IR_MODSYM_FUNCTION && symbol->return_type) {
    return symbol->return_type;
  }
  return function && function->return_type_name
             ? ir_program_lookup_type(program, function->return_type_name)
             : NULL;
}

static int spv_type_is_void(const MtlcType *type, const char *fallback_name) {
  return (type && type->kind == MTLC_TYPE_VOID) ||
         (!type && fallback_name && strcmp(fallback_name, "void") == 0);
}

static SpvDesc call_result_desc(SpvFn *fn, const IRInstruction *in) {
  SpvDesc r = {0};
  MtlcIntrinsic intrinsic = in->intrinsic;
  int bi = 0;
  if (intrinsic == MTLC_INTRINSIC_NONE) {
    long index = spv_function_index(fn->m->program, in->text);
    if (index < 0) {
      r.kind = MTLC_TYPE_VOID;
      return r;
    }
    IRFunction *callee = fn->m->program->functions[(size_t)index];
    const IRModuleSymbol *symbol =
        ir_program_lookup_symbol(fn->m->program, in->text);
    const MtlcType *return_type =
        spv_function_return_type(fn->m->program, callee, symbol);
    if (spv_type_is_void(return_type, callee->return_type_name)) {
      r.kind = MTLC_TYPE_VOID;
    } else {
      r = return_type ? desc_from_type(return_type)
                      : desc_from_typename(callee->return_type_name);
    }
  } else if (sreg_component(intrinsic, &bi) >= 0) {
    r.kind = MTLC_TYPE_UINT32;
    r.is_unsigned = 1;
  } else if (ir_intrinsic_is_subgroup(intrinsic)) {
    r.kind = ir_intrinsic_subgroup_result_kind(intrinsic);
    r.is_unsigned = r.kind == MTLC_TYPE_UINT32;
  } else if (intrinsic == MTLC_INTRINSIC_GPU_F16_BITS_TO_F32) {
    r.kind = MTLC_TYPE_FLOAT32;
  } else if (intrinsic == MTLC_INTRINSIC_GPU_F32_TO_F16_BITS) {
    r.kind = MTLC_TYPE_UINT32;
    r.is_unsigned = 1;
  } else if (intrinsic == MTLC_INTRINSIC_GPU_F32_FROM_BITS) {
    r.kind = MTLC_TYPE_FLOAT32;
  } else if (intrinsic == MTLC_INTRINSIC_GPU_F32_TO_BITS ||
             intrinsic == MTLC_INTRINSIC_GPU_DP4A_U32) {
    r.kind = MTLC_TYPE_UINT32;
    r.is_unsigned = 1;
  } else if (intrinsic == MTLC_INTRINSIC_GPU_DP4A_S32 ||
             intrinsic == MTLC_INTRINSIC_GPU_DP2A_LO_S32 ||
             intrinsic == MTLC_INTRINSIC_GPU_DP2A_HI_S32) {
    r.kind = MTLC_TYPE_INT32;
  } else if (intrinsic == MTLC_INTRINSIC_GPU_DP2A_LO_U32 ||
             intrinsic == MTLC_INTRINSIC_GPU_DP2A_HI_U32 ||
             intrinsic == MTLC_INTRINSIC_GPU_PRMT_B32) {
    r.kind = MTLC_TYPE_UINT32;
    r.is_unsigned = 1;
  } else if (intrinsic == MTLC_INTRINSIC_GPU_H2F_LO ||
             intrinsic == MTLC_INTRINSIC_GPU_H2F_HI ||
             intrinsic == MTLC_INTRINSIC_GPU_BF2F) {
    r.kind = MTLC_TYPE_FLOAT32;
  } else if (intrinsic == MTLC_INTRINSIC_GPU_HADD2 ||
             intrinsic == MTLC_INTRINSIC_GPU_HMUL2 ||
             intrinsic == MTLC_INTRINSIC_GPU_HFMA2 ||
             intrinsic == MTLC_INTRINSIC_GPU_F2H2 ||
             intrinsic == MTLC_INTRINSIC_GPU_F2BF) {
    r.kind = MTLC_TYPE_UINT32;
    r.is_unsigned = 1;
  } else if (intrinsic == MTLC_INTRINSIC_GPU_LOAD4_F32 ||
             intrinsic == MTLC_INTRINSIC_GPU_LOAD4_U32 ||
             intrinsic == MTLC_INTRINSIC_GPU_STORE4_F32 ||
             intrinsic == MTLC_INTRINSIC_GPU_STORE4_U32) {
    r.kind = MTLC_TYPE_VOID;
  } else if (is_math_intrinsic(intrinsic)) {
    r.kind = MTLC_TYPE_FLOAT32;
  } else if (is_atomic_intrinsic(intrinsic)) {
    r.kind = ir_intrinsic_atomic_result_kind(intrinsic);
    r.is_unsigned = 1;
  } else {
    r.kind = MTLC_TYPE_INT32;
  }
  return r;
}

static int is_compare_op(const char *t) {
  return !strcmp(t, "<") || !strcmp(t, ">") || !strcmp(t, "<=") ||
         !strcmp(t, ">=") || !strcmp(t, "==") || !strcmp(t, "!=");
}

/* dominant result kind of a binary op (mirrors PTX emit_binary result class) */
static SpvDesc binary_result_desc(SpvFn *fn, const IRInstruction *in) {
  const char *t = in->text ? in->text : "+";
  SpvDesc la = operand_desc(fn, &in->lhs), ra = operand_desc(fn, &in->rhs);
  SpvDesc dv = {0};
  if (is_compare_op(t)) {
    dv.kind = MTLC_TYPE_INT32;
    dv.is_unsigned = 1;
    return dv;
  }
  if (in->is_float) {
    dv.kind = (in->float_bits == 32) ? MTLC_TYPE_FLOAT32 : MTLC_TYPE_FLOAT64;
  } else if (la.is_ptr || ra.is_ptr) {
    dv.kind = MTLC_TYPE_POINTER;
    dv.is_ptr = 1;
    dv.is_unsigned = 1;
    dv.elem = la.is_ptr ? la.elem : ra.elem;
    dv.address_space = la.is_ptr ? la.address_space : ra.address_space;
  } else if (kind_is_float(la.kind) || kind_is_float(ra.kind)) {
    dv.kind = (la.kind == MTLC_TYPE_FLOAT64 || ra.kind == MTLC_TYPE_FLOAT64)
                  ? MTLC_TYPE_FLOAT64
                  : MTLC_TYPE_FLOAT32;
  } else {
    int b64 = kind_bits(la.kind) == 64 || kind_bits(ra.kind) == 64;
    int u = la.is_unsigned || ra.is_unsigned;
    dv.kind = b64 ? (u ? MTLC_TYPE_UINT64 : MTLC_TYPE_INT64)
                  : (u ? MTLC_TYPE_UINT32 : MTLC_TYPE_INT32);
    dv.is_unsigned = u;
  }
  return dv;
}

/* result descriptor of any instruction that defines dest.name */
static SpvDesc result_desc(SpvFn *fn, const IRInstruction *in) {
  switch (in->op) {
  case IR_OP_BINARY:
    return binary_result_desc(fn, in);
  case IR_OP_UNARY: {
    SpvDesc s = operand_desc(fn, &in->lhs);
    SpvDesc r = {0};
    if (in->is_float) {
      r.kind = (in->float_bits == 32) ? MTLC_TYPE_FLOAT32 : MTLC_TYPE_FLOAT64;
    } else {
      r.kind = kind_bits(s.kind) == 64 ? MTLC_TYPE_INT64 : MTLC_TYPE_INT32;
      r.is_unsigned = s.is_unsigned;
    }
    return r;
  }
  case IR_OP_LOAD: {
    SpvDesc a = operand_desc(fn, &in->lhs);
    MtlcTypeKind elem = a.is_ptr ? a.elem : MTLC_TYPE_VOID;
    if (elem == MTLC_TYPE_VOID) {
      long long sz = (in->rhs.kind == IR_OPERAND_INT) ? in->rhs.int_value : 4;
      if (in->is_float) {
        elem = (sz == 4) ? MTLC_TYPE_FLOAT32 : MTLC_TYPE_FLOAT64;
      } else {
        elem = (sz == 8)   ? MTLC_TYPE_INT64
               : (sz == 2) ? MTLC_TYPE_INT16
               : (sz == 1) ? MTLC_TYPE_UINT8
                           : MTLC_TYPE_INT32;
      }
    }
    SpvDesc r = {0};
    r.kind = elem;
    r.is_unsigned = kind_is_unsigned(elem);
    return r;
  }
  case IR_OP_ASSIGN:
    return operand_desc(fn, &in->lhs);
  case IR_OP_CAST:
    return in->value_type ? desc_from_type(in->value_type)
                          : desc_from_typename(in->text);
  case IR_OP_CALL:
    return call_result_desc(fn, in);
  default: {
    SpvDesc r = {0};
    r.kind = MTLC_TYPE_INT32;
    return r;
  }
  }
}

/* ---- conversions ---- */
/* convert value `id` of kind `from` to kind `to`; returns a new id (or the same
 * id when the representation is identical, e.g. int32 vs uint32). */
static uint32_t convert(SpvFn *fn, uint32_t id, MtlcTypeKind from, int from_uns,
                        MtlcTypeKind to) {
  SpvMod *m = fn->m;
  if (kind_type(m, from) == kind_type(m, to) &&
      kind_is_float(from) == kind_is_float(to)) {
    return id;
  }
  uint32_t tt = kind_type(m, to);
  uint32_t r = new_id(m);
  int ff = kind_is_float(from), tf = kind_is_float(to);
  uint16_t op;
  if (!ff && !tf) {
    op = from_uns ? Op_UConvert : Op_SConvert;
  } else if (!ff && tf) {
    op = from_uns ? Op_ConvertUToF : Op_ConvertSToF;
  } else if (ff && !tf) {
    op = kind_is_unsigned(to) ? Op_ConvertFToU : Op_ConvertFToS;
  } else {
    op = Op_FConvert;
  }
  emitv(&m->functions, op, 3, tt, r, id);
  return r;
}

/* Resolve an operand to a value id of kind `want`, materializing immediates and
 * inserting conversions (loads named values from their variables). */
static uint32_t materialize(SpvFn *fn, const IROperand *op, MtlcTypeKind want) {
  SpvMod *m = fn->m;
  if (op->kind == IR_OPERAND_INT) {
    if (kind_is_float(want)) {
      return const_float(m, want, (double)op->int_value);
    }
    return const_scalar_int(m, want, op->int_value);
  }
  if (op->kind == IR_OPERAND_FLOAT) {
    MtlcTypeKind src = (op->float_bits == 32) ? MTLC_TYPE_FLOAT32 : MTLC_TYPE_FLOAT64;
    uint32_t id = const_float(m, src, op->float_value);
    return convert(fn, id, src, 0, want);
  }
  SpvBind *b = find_bind(fn, op->name);
  if (!b) {
    mod_error(m, "SPIR-V: use of undefined value '%s'", op->name ? op->name : "?");
    return const_scalar_int(m, MTLC_TYPE_INT32, 0);
  }
  uint32_t ldt = kind_type(m, b->d.kind);
  uint32_t v = new_id(m);
  emitv(&m->functions, Op_Load, 3, ldt, v, b->var_id);
  return convert(fn, v, b->d.kind, b->d.is_unsigned, want);
}

static void store_name(SpvFn *fn, const char *name, uint32_t value) {
  SpvBind *b = find_bind(fn, name);
  if (!b) return;
  emitv(&fn->m->functions, Op_Store, 2, b->var_id, value);
}

/* ---- load/store through a computed pointer (carried as int64) ---- */
static MtlcTypeKind load_store_elem(SpvFn *fn, const IROperand *addr,
                                    const IRInstruction *in) {
  SpvDesc a = operand_desc(fn, addr);
  MtlcTypeKind elem = a.is_ptr ? a.elem : MTLC_TYPE_VOID;
  if (elem == MTLC_TYPE_VOID) {
    long long sz = (in->rhs.kind == IR_OPERAND_INT) ? in->rhs.int_value : 4;
    if (in->is_float) {
      elem = (sz == 4) ? MTLC_TYPE_FLOAT32 : MTLC_TYPE_FLOAT64;
    } else {
      elem = (sz == 8)   ? MTLC_TYPE_INT64
             : (sz == 2) ? MTLC_TYPE_INT16
             : (sz == 1) ? MTLC_TYPE_UINT8
                         : MTLC_TYPE_INT32;
    }
  }
  return elem;
}
static uint32_t typed_memory_ptr(SpvFn *fn, uint32_t addr64, MtlcTypeKind elem,
                                 MtlcAddressSpace address_space) {
  SpvMod *m = fn->m;
  int sc = spv_storage_class(address_space);
  if (sc < 0) {
    mod_error(m, "SPIR-V: invalid address space %d", (int)address_space);
    return 0;
  }
  uint32_t pt = type_pointer(m, sc, kind_type(m, elem));
  uint32_t p = new_id(m);
  emitv(&m->functions, Op_ConvertUToPtr, 3, pt, p, addr64);
  return p;
}

static void emit_async_copy(SpvFn *fn, const IRInstruction *in) {
  SpvMod *m = fn->m;
  if (!in || in->argument_count != 2 ||
      in->async_copy_element_count == 0) {
    mod_error(m, "SPIR-V: invalid asynchronous-copy instruction");
    return;
  }
  SpvDesc destination = operand_desc(fn, &in->arguments[0]);
  SpvDesc source = operand_desc(fn, &in->arguments[1]);
  if (!destination.is_ptr || !source.is_ptr ||
      destination.address_space != MTLC_ADDRESS_SPACE_WORKGROUP ||
      (source.address_space != MTLC_ADDRESS_SPACE_GLOBAL &&
       source.address_space != MTLC_ADDRESS_SPACE_GENERIC) ||
      destination.elem != source.elem || kind_bits(destination.elem) <= 0 ||
      (kind_bits(destination.elem) & 7) != 0) {
    mod_error(m,
              "SPIR-V: async copy requires matching global-to-workgroup scalar pointers");
    return;
  }
  uint32_t destination_base =
      materialize(fn, &in->arguments[0], MTLC_TYPE_POINTER);
  uint32_t source_base =
      materialize(fn, &in->arguments[1], MTLC_TYPE_POINTER);
  uint32_t u64 = type_int(m, 64);
  uint32_t element_type = kind_type(m, destination.elem);
  size_t element_size = (size_t)kind_bits(destination.elem) / 8u;
  for (uint32_t element = 0; element < in->async_copy_element_count;
       element++) {
    size_t byte_offset = (size_t)element * element_size;
    uint32_t destination_address = destination_base;
    uint32_t source_address = source_base;
    if (byte_offset != 0) {
      uint32_t offset =
          const_scalar_int(m, MTLC_TYPE_UINT64, (long long)byte_offset);
      destination_address = new_id(m);
      source_address = new_id(m);
      emitv(&m->functions, Op_IAdd, 4, u64, destination_address,
            destination_base, offset);
      emitv(&m->functions, Op_IAdd, 4, u64, source_address, source_base,
            offset);
    }
    uint32_t destination_pointer = typed_memory_ptr(
        fn, destination_address, destination.elem,
        MTLC_ADDRESS_SPACE_WORKGROUP);
    uint32_t source_pointer = typed_memory_ptr(
        fn, source_address, source.elem, source.address_space);
    uint32_t value = new_id(m);
    emitv(&m->functions, Op_Load, 5, element_type, value, source_pointer,
          (unsigned)MemAccess_Aligned, (unsigned)element_size);
    emitv(&m->functions, Op_Store, 4, destination_pointer, value,
          (unsigned)MemAccess_Aligned, (unsigned)element_size);
  }
}

/* ============================ op lowering ============================ */
static void emit_binary(SpvFn *fn, const IRInstruction *in) {
  SpvMod *m = fn->m;
  const char *t = in->text ? in->text : "+";
  SpvDesc la = operand_desc(fn, &in->lhs), ra = operand_desc(fn, &in->rhs);

  if (is_compare_op(t)) {
    int is_float = kind_is_float(la.kind) || kind_is_float(ra.kind) || in->is_float;
    MtlcTypeKind c;
    int uns = 0;
    if (is_float) {
      c = (la.kind == MTLC_TYPE_FLOAT64 || ra.kind == MTLC_TYPE_FLOAT64)
              ? MTLC_TYPE_FLOAT64
              : MTLC_TYPE_FLOAT32;
    } else {
      int b64 = kind_bits(la.kind) == 64 || kind_bits(ra.kind) == 64;
      uns = la.is_unsigned || ra.is_unsigned;
      c = b64 ? MTLC_TYPE_INT64 : MTLC_TYPE_INT32;
    }
    uint32_t a = materialize(fn, &in->lhs, c);
    uint32_t b = materialize(fn, &in->rhs, c);
    uint16_t op;
    if (is_float) {
      if (!strcmp(t, "==")) op = Op_FOrdEqual;
      else if (!strcmp(t, "!=")) op = Op_FUnordNotEqual;
      else if (!strcmp(t, "<")) op = Op_FOrdLessThan;
      else if (!strcmp(t, ">")) op = Op_FOrdGreaterThan;
      else if (!strcmp(t, "<=")) op = Op_FOrdLessThanEqual;
      else op = Op_FOrdGreaterThanEqual;
    } else {
      if (!strcmp(t, "==")) op = Op_IEqual;
      else if (!strcmp(t, "!=")) op = Op_INotEqual;
      else if (!strcmp(t, "<")) op = uns ? Op_ULessThan : Op_SLessThan;
      else if (!strcmp(t, ">")) op = uns ? Op_UGreaterThan : Op_SGreaterThan;
      else if (!strcmp(t, "<=")) op = uns ? Op_ULessThanEqual : Op_SLessThanEqual;
      else op = uns ? Op_UGreaterThanEqual : Op_SGreaterThanEqual;
    }
    uint32_t bt = type_bool(m);
    uint32_t cond = new_id(m);
    emitv(&m->functions, op, 4, bt, cond, a, b);
    /* materialize the boolean as a 0/1 int32 result (matches PTX selp) */
    uint32_t i32 = type_int(m, 32);
    uint32_t one = const_scalar_int(m, MTLC_TYPE_INT32, 1);
    uint32_t zero = const_scalar_int(m, MTLC_TYPE_INT32, 0);
    uint32_t res = new_id(m);
    emitv(&m->functions, Op_Select, 5, i32, res, cond, one, zero);
    if (in->dest.name) store_name(fn, in->dest.name, res);
    return;
  }

  SpvDesc rd = binary_result_desc(fn, in);
  MtlcTypeKind rk = rd.kind;
  uint32_t rt = kind_type(m, rk);
  int is_float = kind_is_float(rk);
  uint32_t a = materialize(fn, &in->lhs, rk);
  uint32_t b = materialize(fn, &in->rhs, rk);
  uint32_t res = new_id(m);
  uint16_t op = 0;

  if (!strcmp(t, "+")) op = is_float ? Op_FAdd : Op_IAdd;
  else if (!strcmp(t, "-")) op = is_float ? Op_FSub : Op_ISub;
  else if (!strcmp(t, "*")) op = is_float ? Op_FMul : Op_IMul;
  else if (!strcmp(t, "/")) op = is_float ? Op_FDiv : (rd.is_unsigned ? Op_UDiv : Op_SDiv);
  else if (!strcmp(t, "%")) op = is_float ? Op_FRem : (rd.is_unsigned ? Op_UMod : Op_SRem);
  else if (!strcmp(t, "&") || !strcmp(t, "&&")) op = Op_BitwiseAnd;
  else if (!strcmp(t, "|") || !strcmp(t, "||")) op = Op_BitwiseOr;
  else if (!strcmp(t, "^")) op = Op_BitwiseXor;
  else if (!strcmp(t, "<<")) op = Op_ShiftLeftLogical;
  else if (!strcmp(t, ">>")) op = la.is_unsigned ? Op_ShiftRightLogical : Op_ShiftRightArithmetic;
  else {
    mod_error(m, "SPIR-V: unsupported binary op '%s'", t);
    return;
  }
  emitv(&m->functions, op, 4, rt, res, a, b);
  if (in->dest.name) store_name(fn, in->dest.name, res);
}

static void emit_unary(SpvFn *fn, const IRInstruction *in) {
  SpvMod *m = fn->m;
  const char *t = in->text ? in->text : "";
  SpvDesc rd = result_desc(fn, in);
  MtlcTypeKind rk = rd.kind;
  uint32_t rt = kind_type(m, rk);
  int is_float = kind_is_float(rk);
  uint32_t s = materialize(fn, &in->lhs, rk);
  uint32_t res = new_id(m);
  if (!strcmp(t, "-")) {
    emitv(&m->functions, is_float ? Op_FNegate : Op_SNegate, 3, rt, res, s);
  } else if (!strcmp(t, "~")) {
    emitv(&m->functions, Op_Not, 3, rt, res, s);
  } else if (!strcmp(t, "!")) {
    uint32_t bt = type_bool(m);
    uint32_t zero = is_float ? const_float(m, rk, 0.0) : const_scalar_int(m, rk, 0);
    uint32_t cond = new_id(m);
    emitv(&m->functions, is_float ? Op_FOrdEqual : Op_IEqual, 4, bt, cond, s, zero);
    uint32_t one = const_scalar_int(m, MTLC_TYPE_INT32, 1);
    uint32_t z0 = const_scalar_int(m, MTLC_TYPE_INT32, 0);
    uint32_t i32 = type_int(m, 32);
    res = new_id(m);
    emitv(&m->functions, Op_Select, 5, i32, res, cond, one, z0);
  } else {
    mod_error(m, "SPIR-V: unsupported unary op '%s'", t);
    return;
  }
  if (in->dest.name) store_name(fn, in->dest.name, res);
}

static void emit_load(SpvFn *fn, const IRInstruction *in) {
  SpvMod *m = fn->m;
  MtlcTypeKind elem = load_store_elem(fn, &in->lhs, in);
  SpvDesc ad = operand_desc(fn, &in->lhs);
  uint32_t addr = materialize(fn, &in->lhs, MTLC_TYPE_POINTER);
  uint32_t p = typed_memory_ptr(fn, addr, elem, ad.address_space);
  uint32_t et = kind_type(m, elem);
  uint32_t v = new_id(m);
  int align = kind_bits(elem) / 8;
  emitv(&m->functions, Op_Load, 5, et, v, p, (unsigned)MemAccess_Aligned,
        (unsigned)align);
  if (in->dest.name) store_name(fn, in->dest.name, v);
}

static void emit_store(SpvFn *fn, const IRInstruction *in) {
  SpvMod *m = fn->m;
  MtlcTypeKind elem = load_store_elem(fn, &in->dest, in);
  SpvDesc ad = operand_desc(fn, &in->dest);
  uint32_t addr = materialize(fn, &in->dest, MTLC_TYPE_POINTER);
  uint32_t val = materialize(fn, &in->lhs, elem);
  if (ad.address_space == MTLC_ADDRESS_SPACE_CONSTANT) {
    mod_error(m, "SPIR-V: store to constant address space");
    return;
  }
  uint32_t p = typed_memory_ptr(fn, addr, elem, ad.address_space);
  int align = kind_bits(elem) / 8;
  emitv(&m->functions, Op_Store, 4, p, val, (unsigned)MemAccess_Aligned,
        (unsigned)align);
}

static void emit_cast(SpvFn *fn, const IRInstruction *in) {
  SpvDesc target = in->value_type ? desc_from_type(in->value_type)
                                  : desc_from_typename(in->text);
  uint32_t id = materialize(fn, &in->lhs, target.kind);
  if (in->dest.name) store_name(fn, in->dest.name, id);
}

static void emit_call(SpvFn *fn, const IRInstruction *in) {
  SpvMod *m = fn->m;
  const char *callee = in->text;
  MtlcIntrinsic intrinsic = in->intrinsic;
  int bi = 0;
  int comp = sreg_component(intrinsic, &bi);
  if (comp >= 0) {
    uint32_t var = builtin_var(m, bi);
    track_builtin(fn, bi, var);
    uint32_t v3 = type_vec3_ulong(m);
    uint32_t u64 = type_int(m, 64);
    uint32_t loaded = new_id(m);
    emitv(&m->functions, Op_Load, 3, v3, loaded, var);
    uint32_t c = new_id(m);
    emitv(&m->functions, Op_CompositeExtract, 4, u64, c, loaded, (unsigned)comp);
    uint32_t u32 = type_int(m, 32);
    uint32_t r = new_id(m);
    emitv(&m->functions, Op_UConvert, 3, u32, r, c);
    if (in->dest.name) store_name(fn, in->dest.name, r);
    return;
  }
  if (intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_LOCAL_ID ||
      intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_SIZE) {
    int builtin = intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_LOCAL_ID
                      ? BI_SubgroupLocalInvocationId
                      : BI_SubgroupSize;
    uint32_t var = builtin_var(m, builtin);
    uint32_t u32 = type_int(m, 32);
    uint32_t result = new_id(m);
    track_builtin(fn, builtin, var);
    m->use_subgroups = 1;
    emitv(&m->functions, Op_Load, 3, u32, result, var);
    if (in->dest.name) store_name(fn, in->dest.name, result);
    return;
  }
  if ((intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_BROADCAST_U32 ||
       intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_BROADCAST_F32) &&
      in->argument_count >= 2) {
    MtlcTypeKind value_kind =
        intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_BROADCAST_F32
            ? MTLC_TYPE_FLOAT32
            : MTLC_TYPE_UINT32;
    uint32_t result_type = kind_type(m, value_kind);
    uint32_t value = materialize(fn, &in->arguments[0], value_kind);
    uint32_t source_lane =
        materialize(fn, &in->arguments[1], MTLC_TYPE_UINT32);
    uint32_t result = new_id(m);
    uint32_t scope = const_u32(m, Scope_Subgroup);
    m->use_subgroups = 1;
    emitv(&m->functions, Op_GroupBroadcast, 5, result_type, result, scope,
          value, source_lane);
    if (in->dest.name) store_name(fn, in->dest.name, result);
    return;
  }
  if (intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_SHUFFLE_U32 ||
      intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_SHUFFLE_F32) {
    mod_error(m,
              "SPIR-V OpenCL 2.0 does not provide non-uniform subgroup "
              "shuffle; select a profile with GroupNonUniformShuffle");
    return;
  }
  if (intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_BALLOT_WORD &&
      in->argument_count >= 2) {
    uint32_t predicate_value =
        materialize(fn, &in->arguments[0], MTLC_TYPE_BOOL);
    uint32_t word =
        materialize(fn, &in->arguments[1], MTLC_TYPE_UINT32);
    uint32_t u32 = type_int(m, 32);
    uint32_t bt = type_bool(m);
    uint32_t predicate = new_id(m);
    uint32_t v4u32 = type_vec4_uint(m);
    uint32_t ballot = new_id(m);
    uint32_t valid = new_id(m);
    uint32_t safe_word = new_id(m);
    uint32_t extracted = new_id(m);
    uint32_t result = new_id(m);
    uint32_t zero = const_u32(m, 0);
    uint32_t four = const_u32(m, 4);
    emitv(&m->functions, Op_INotEqual, 4, bt, predicate,
          predicate_value, const_scalar_int(m, MTLC_TYPE_BOOL, 0));
    m->use_subgroups = 1;
    m->use_subgroup_ballot = 1;
    emitv(&m->functions, Op_SubgroupBallotKHR, 3,
          v4u32, ballot, predicate);
    emitv(&m->functions, Op_ULessThan, 4, bt, valid, word, four);
    /* Vector extraction is undefined for an out-of-range index. Select zero
     * before extraction and then zero the observable result for word >= 4. */
    emitv(&m->functions, Op_Select, 5, u32, safe_word, valid, word, zero);
    emitv(&m->functions, Op_VectorExtractDynamic, 4,
          u32, extracted, ballot, safe_word);
    emitv(&m->functions, Op_Select, 5,
          u32, result, valid, extracted, zero);
    if (in->dest.name) store_name(fn, in->dest.name, result);
    return;
  }
  if ((intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_ANY ||
       intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_ALL) &&
      in->argument_count >= 1) {
    uint32_t predicate_value =
        materialize(fn, &in->arguments[0], MTLC_TYPE_BOOL);
    uint32_t result_type = type_bool(m);
    uint32_t predicate = new_id(m);
    uint32_t vote_result = new_id(m);
    uint32_t result = new_id(m);
    emitv(&m->functions, Op_INotEqual, 4, result_type, predicate,
          predicate_value, const_scalar_int(m, MTLC_TYPE_BOOL, 0));
    m->use_subgroups = 1;
    m->use_subgroup_vote = 1;
    emitv(&m->functions,
          intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_ANY
              ? Op_SubgroupAnyKHR
              : Op_SubgroupAllKHR,
          3, result_type, vote_result, predicate);
    emitv(&m->functions, Op_Select, 5,
          kind_type(m, MTLC_TYPE_BOOL), result, vote_result,
          const_scalar_int(m, MTLC_TYPE_BOOL, 1),
          const_scalar_int(m, MTLC_TYPE_BOOL, 0));
    if (in->dest.name) store_name(fn, in->dest.name, result);
    return;
  }
  if ((intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_ADD_U32 ||
       intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_ADD_F32 ||
       intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MIN_U32 ||
       intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MIN_F32 ||
       intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MAX_U32 ||
       intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MAX_F32 ||
       intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_INCLUSIVE_ADD_U32 ||
       intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_INCLUSIVE_ADD_F32 ||
       intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_EXCLUSIVE_ADD_U32 ||
       intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_EXCLUSIVE_ADD_F32) &&
      in->argument_count >= 1) {
    int is_float = intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_ADD_F32 ||
                   intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MIN_F32 ||
                   intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MAX_F32 ||
                   intrinsic ==
                       MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_INCLUSIVE_ADD_F32 ||
                   intrinsic ==
                       MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_EXCLUSIVE_ADD_F32;
    int is_min = intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MIN_U32 ||
                 intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MIN_F32;
    int is_max = intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MAX_U32 ||
                 intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_REDUCE_MAX_F32;
    unsigned group_operation =
        (intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_INCLUSIVE_ADD_U32 ||
         intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_INCLUSIVE_ADD_F32)
            ? 1u
        : (intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_EXCLUSIVE_ADD_U32 ||
           intrinsic == MTLC_INTRINSIC_GPU_SUBGROUP_SCAN_EXCLUSIVE_ADD_F32)
            ? 2u
            : 0u;
    unsigned opcode = is_min ? (is_float ? Op_GroupFMin : Op_GroupUMin)
                      : is_max ? (is_float ? Op_GroupFMax : Op_GroupUMax)
                               : (is_float ? Op_GroupFAdd : Op_GroupIAdd);
    MtlcTypeKind value_kind = is_float ? MTLC_TYPE_FLOAT32 : MTLC_TYPE_UINT32;
    uint32_t result_type = kind_type(m, value_kind);
    uint32_t value = materialize(fn, &in->arguments[0], value_kind);
    uint32_t result = new_id(m);
    uint32_t scope = const_u32(m, Scope_Subgroup);
    m->use_subgroups = 1;
    /* GroupOperation is a literal: Reduce=0, InclusiveScan=1,
     * ExclusiveScan=2. */
    emitv(&m->functions, opcode, 5, result_type, result, scope,
          group_operation, value);
    if (in->dest.name) store_name(fn, in->dest.name, result);
    return;
  }
  if (intrinsic == MTLC_INTRINSIC_GPU_WORKGROUP_BARRIER) {
    emit_workgroup_barrier(fn, MTLC_MEMORY_ORDER_SEQ_CST,
                           MTLC_MEMORY_REGION_WORKGROUP);
    return;
  }
  if (intrinsic == MTLC_INTRINSIC_GPU_F16_BITS_TO_F32 &&
      in->argument_count >= 1) {
    /* reinterpret a uint16 fp16 bit-pattern (arrives zero-extended in an int)
     * as float32: truncate to 16 bits, bitcast to half, convert to float. */
    uint32_t a = materialize(fn, &in->arguments[0], MTLC_TYPE_UINT32);
    uint32_t i16 = type_int(m, 16);
    uint32_t h = type_float(m, 16);
    uint32_t f32 = type_float(m, 32);
    uint32_t t16 = new_id(m);
    emitv(&m->functions, Op_UConvert, 3, i16, t16, a);
    uint32_t half = new_id(m);
    emitv(&m->functions, Op_Bitcast, 3, h, half, t16);
    uint32_t r = new_id(m);
    emitv(&m->functions, Op_FConvert, 3, f32, r, half);
    if (in->dest.name) store_name(fn, in->dest.name, r);
    return;
  }
  if (intrinsic == MTLC_INTRINSIC_GPU_F32_TO_F16_BITS &&
      in->argument_count >= 1) {
    uint32_t a = materialize(fn, &in->arguments[0], MTLC_TYPE_FLOAT32);
    uint32_t h = type_float(m, 16);
    uint32_t i16 = type_int(m, 16);
    uint32_t u32 = type_int(m, 32);
    uint32_t half = new_id(m);
    emitv(&m->functions, Op_FConvert, 3, h, half, a);
    uint32_t b16 = new_id(m);
    emitv(&m->functions, Op_Bitcast, 3, i16, b16, half);
    uint32_t r = new_id(m);
    emitv(&m->functions, Op_UConvert, 3, u32, r, b16);
    if (in->dest.name) store_name(fn, in->dest.name, r);
    return;
  }
  if (intrinsic == MTLC_INTRINSIC_GPU_F32_FROM_BITS &&
      in->argument_count >= 1) {
    uint32_t a = materialize(fn, &in->arguments[0], MTLC_TYPE_UINT32);
    uint32_t f32 = type_float(m, 32);
    uint32_t r = new_id(m);
    emitv(&m->functions, Op_Bitcast, 3, f32, r, a);
    if (in->dest.name) store_name(fn, in->dest.name, r);
    return;
  }
  if (intrinsic == MTLC_INTRINSIC_GPU_F32_TO_BITS &&
      in->argument_count >= 1) {
    uint32_t a = materialize(fn, &in->arguments[0], MTLC_TYPE_FLOAT32);
    uint32_t u32 = type_int(m, 32);
    uint32_t r = new_id(m);
    emitv(&m->functions, Op_Bitcast, 3, u32, r, a);
    if (in->dest.name) store_name(fn, in->dest.name, r);
    return;
  }
  if ((intrinsic == MTLC_INTRINSIC_GPU_DP4A_U32 ||
       intrinsic == MTLC_INTRINSIC_GPU_DP4A_S32) &&
      in->argument_count >= 3) {
    /* No portable SPIR-V 1.0/OpenCL dot-product capability: replay the exact
     * semantics as four byte extractions, multiplies, and adds. Unsigned bytes
     * mask; signed bytes sign-extend via shift-left + arithmetic shift-right. */
    int is_signed = intrinsic == MTLC_INTRINSIC_GPU_DP4A_S32;
    MtlcTypeKind kind = is_signed ? MTLC_TYPE_INT32 : MTLC_TYPE_UINT32;
    uint32_t a = materialize(fn, &in->arguments[0], kind);
    uint32_t b = materialize(fn, &in->arguments[1], kind);
    uint32_t acc = materialize(fn, &in->arguments[2], kind);
    uint32_t i32 = type_int(m, 32);
    for (unsigned byte = 0; byte < 4; byte++) {
      uint32_t ea;
      uint32_t eb;
      if (is_signed) {
        uint32_t left = const_u32(m, 24u - byte * 8u);
        uint32_t right = const_u32(m, 24u);
        uint32_t la = new_id(m);
        emitv(&m->functions, Op_ShiftLeftLogical, 4, i32, la, a, left);
        ea = new_id(m);
        emitv(&m->functions, Op_ShiftRightArithmetic, 4, i32, ea, la, right);
        uint32_t lb = new_id(m);
        emitv(&m->functions, Op_ShiftLeftLogical, 4, i32, lb, b, left);
        eb = new_id(m);
        emitv(&m->functions, Op_ShiftRightArithmetic, 4, i32, eb, lb, right);
      } else {
        uint32_t shift = const_u32(m, byte * 8u);
        uint32_t mask = const_u32(m, 0xFFu);
        uint32_t sa = new_id(m);
        emitv(&m->functions, Op_ShiftRightLogical, 4, i32, sa, a, shift);
        ea = new_id(m);
        emitv(&m->functions, Op_BitwiseAnd, 4, i32, ea, sa, mask);
        uint32_t sb = new_id(m);
        emitv(&m->functions, Op_ShiftRightLogical, 4, i32, sb, b, shift);
        eb = new_id(m);
        emitv(&m->functions, Op_BitwiseAnd, 4, i32, eb, sb, mask);
      }
      uint32_t product = new_id(m);
      emitv(&m->functions, Op_IMul, 4, i32, product, ea, eb);
      uint32_t next = new_id(m);
      emitv(&m->functions, Op_IAdd, 4, i32, next, acc, product);
      acc = next;
    }
    if (in->dest.name) store_name(fn, in->dest.name, acc);
    return;
  }
  if ((intrinsic == MTLC_INTRINSIC_GPU_DP2A_LO_U32 ||
       intrinsic == MTLC_INTRINSIC_GPU_DP2A_LO_S32 ||
       intrinsic == MTLC_INTRINSIC_GPU_DP2A_HI_U32 ||
       intrinsic == MTLC_INTRINSIC_GPU_DP2A_HI_S32) &&
      in->argument_count >= 3) {
    /* No portable SPIR-V mixed-width dot product: replay the exact semantics.
     * Two 16-bit halves of a against the low or high byte pair of b. */
    int is_signed = intrinsic == MTLC_INTRINSIC_GPU_DP2A_LO_S32 ||
                    intrinsic == MTLC_INTRINSIC_GPU_DP2A_HI_S32;
    int is_high = intrinsic == MTLC_INTRINSIC_GPU_DP2A_HI_U32 ||
                  intrinsic == MTLC_INTRINSIC_GPU_DP2A_HI_S32;
    MtlcTypeKind kind = is_signed ? MTLC_TYPE_INT32 : MTLC_TYPE_UINT32;
    uint32_t a = materialize(fn, &in->arguments[0], kind);
    uint32_t b = materialize(fn, &in->arguments[1], kind);
    uint32_t acc = materialize(fn, &in->arguments[2], kind);
    uint32_t i32 = type_int(m, 32);
    for (unsigned half = 0; half < 2; half++) {
      uint32_t ea;
      uint32_t eb;
      unsigned byte_index = (is_high ? 2u : 0u) + half;
      if (is_signed) {
        uint32_t half_left = const_u32(m, 16u - half * 16u);
        uint32_t half_right = const_u32(m, 16u);
        uint32_t byte_left = const_u32(m, 24u - byte_index * 8u);
        uint32_t byte_right = const_u32(m, 24u);
        uint32_t la = new_id(m);
        emitv(&m->functions, Op_ShiftLeftLogical, 4, i32, la, a, half_left);
        ea = new_id(m);
        emitv(&m->functions, Op_ShiftRightArithmetic, 4, i32, ea, la,
              half_right);
        uint32_t lb = new_id(m);
        emitv(&m->functions, Op_ShiftLeftLogical, 4, i32, lb, b, byte_left);
        eb = new_id(m);
        emitv(&m->functions, Op_ShiftRightArithmetic, 4, i32, eb, lb,
              byte_right);
      } else {
        uint32_t half_shift = const_u32(m, half * 16u);
        uint32_t half_mask = const_u32(m, 0xFFFFu);
        uint32_t byte_shift = const_u32(m, byte_index * 8u);
        uint32_t byte_mask = const_u32(m, 0xFFu);
        uint32_t sa = new_id(m);
        emitv(&m->functions, Op_ShiftRightLogical, 4, i32, sa, a, half_shift);
        ea = new_id(m);
        emitv(&m->functions, Op_BitwiseAnd, 4, i32, ea, sa, half_mask);
        uint32_t sb = new_id(m);
        emitv(&m->functions, Op_ShiftRightLogical, 4, i32, sb, b, byte_shift);
        eb = new_id(m);
        emitv(&m->functions, Op_BitwiseAnd, 4, i32, eb, sb, byte_mask);
      }
      uint32_t product = new_id(m);
      emitv(&m->functions, Op_IMul, 4, i32, product, ea, eb);
      uint32_t next = new_id(m);
      emitv(&m->functions, Op_IAdd, 4, i32, next, acc, product);
      acc = next;
    }
    if (in->dest.name) store_name(fn, in->dest.name, acc);
    return;
  }
  if ((intrinsic == MTLC_INTRINSIC_GPU_HADD2 ||
       intrinsic == MTLC_INTRINSIC_GPU_HMUL2 ||
       intrinsic == MTLC_INTRINSIC_GPU_HFMA2) &&
      in->argument_count >= 2) {
    /* SPIR-V 1.0 OpenCL has no packed-half arithmetic type, so each lane is
     * converted, computed in f16, and repacked. Same values, one lane at a
     * time; the packed instruction is a PTX-side win only. */
    int is_fma = intrinsic == MTLC_INTRINSIC_GPU_HFMA2;
    uint32_t a = materialize(fn, &in->arguments[0], MTLC_TYPE_UINT32);
    uint32_t b = materialize(fn, &in->arguments[1], MTLC_TYPE_UINT32);
    uint32_t c = is_fma ? materialize(fn, &in->arguments[2], MTLC_TYPE_UINT32)
                        : 0;
    uint32_t i32 = type_int(m, 32);
    uint32_t i16 = type_int(m, 16);
    uint32_t h = type_float(m, 16);
    uint32_t result = const_u32(m, 0);
    for (unsigned lane = 0; lane < 2; lane++) {
      uint32_t shift = const_u32(m, lane * 16u);
      uint32_t mask = const_u32(m, 0xFFFFu);
      uint32_t values[3];
      unsigned operand_count = is_fma ? 3u : 2u;
      uint32_t sources[3] = {a, b, c};
      for (unsigned operand = 0; operand < operand_count; operand++) {
        uint32_t moved = new_id(m);
        emitv(&m->functions, Op_ShiftRightLogical, 4, i32, moved,
              sources[operand], shift);
        uint32_t masked = new_id(m);
        emitv(&m->functions, Op_BitwiseAnd, 4, i32, masked, moved, mask);
        uint32_t narrowed = new_id(m);
        emitv(&m->functions, Op_UConvert, 3, i16, narrowed, masked);
        values[operand] = new_id(m);
        emitv(&m->functions, Op_Bitcast, 3, h, values[operand], narrowed);
      }
      uint32_t computed = new_id(m);
      if (intrinsic == MTLC_INTRINSIC_GPU_HADD2) {
        emitv(&m->functions, Op_FAdd, 4, h, computed, values[0], values[1]);
      } else {
        uint32_t product = new_id(m);
        emitv(&m->functions, Op_FMul, 4, h, product, values[0], values[1]);
        if (is_fma) {
          emitv(&m->functions, Op_FAdd, 4, h, computed, product, values[2]);
        } else {
          computed = product;
        }
      }
      uint32_t back = new_id(m);
      emitv(&m->functions, Op_Bitcast, 3, i16, back, computed);
      uint32_t widened = new_id(m);
      emitv(&m->functions, Op_UConvert, 3, i32, widened, back);
      uint32_t placed = new_id(m);
      emitv(&m->functions, Op_ShiftLeftLogical, 4, i32, placed, widened, shift);
      uint32_t merged = new_id(m);
      emitv(&m->functions, Op_BitwiseOr, 4, i32, merged, result, placed);
      result = merged;
    }
    if (in->dest.name) store_name(fn, in->dest.name, result);
    return;
  }
  if ((intrinsic == MTLC_INTRINSIC_GPU_H2F_LO ||
       intrinsic == MTLC_INTRINSIC_GPU_H2F_HI) &&
      in->argument_count >= 1) {
    uint32_t a = materialize(fn, &in->arguments[0], MTLC_TYPE_UINT32);
    uint32_t i32 = type_int(m, 32);
    uint32_t i16 = type_int(m, 16);
    uint32_t h = type_float(m, 16);
    uint32_t f32 = type_float(m, 32);
    uint32_t moved = a;
    if (intrinsic == MTLC_INTRINSIC_GPU_H2F_HI) {
      moved = new_id(m);
      emitv(&m->functions, Op_ShiftRightLogical, 4, i32, moved, a,
            const_u32(m, 16u));
    }
    uint32_t narrowed = new_id(m);
    emitv(&m->functions, Op_UConvert, 3, i16, narrowed, moved);
    uint32_t half = new_id(m);
    emitv(&m->functions, Op_Bitcast, 3, h, half, narrowed);
    uint32_t widened = new_id(m);
    emitv(&m->functions, Op_FConvert, 3, f32, widened, half);
    if (in->dest.name) store_name(fn, in->dest.name, widened);
    return;
  }
  if (intrinsic == MTLC_INTRINSIC_GPU_F2H2 && in->argument_count >= 2) {
    uint32_t i32 = type_int(m, 32);
    uint32_t i16 = type_int(m, 16);
    uint32_t h = type_float(m, 16);
    uint32_t result = const_u32(m, 0);
    for (unsigned lane = 0; lane < 2; lane++) {
      uint32_t value = materialize(fn, &in->arguments[lane], MTLC_TYPE_FLOAT32);
      uint32_t narrowed = new_id(m);
      emitv(&m->functions, Op_FConvert, 3, h, narrowed, value);
      uint32_t bits = new_id(m);
      emitv(&m->functions, Op_Bitcast, 3, i16, bits, narrowed);
      uint32_t widened = new_id(m);
      emitv(&m->functions, Op_UConvert, 3, i32, widened, bits);
      uint32_t placed = new_id(m);
      emitv(&m->functions, Op_ShiftLeftLogical, 4, i32, placed, widened,
            const_u32(m, lane * 16u));
      uint32_t merged = new_id(m);
      emitv(&m->functions, Op_BitwiseOr, 4, i32, merged, result, placed);
      result = merged;
    }
    if (in->dest.name) store_name(fn, in->dest.name, result);
    return;
  }
  if (intrinsic == MTLC_INTRINSIC_GPU_BF2F && in->argument_count >= 1) {
    uint32_t a = materialize(fn, &in->arguments[0], MTLC_TYPE_UINT32);
    uint32_t i32 = type_int(m, 32);
    uint32_t f32 = type_float(m, 32);
    uint32_t shifted = new_id(m);
    emitv(&m->functions, Op_ShiftLeftLogical, 4, i32, shifted, a,
          const_u32(m, 16u));
    uint32_t result = new_id(m);
    emitv(&m->functions, Op_Bitcast, 3, f32, result, shifted);
    if (in->dest.name) store_name(fn, in->dest.name, result);
    return;
  }
  if (intrinsic == MTLC_INTRINSIC_GPU_F2BF && in->argument_count >= 1) {
    uint32_t a = materialize(fn, &in->arguments[0], MTLC_TYPE_FLOAT32);
    uint32_t i32 = type_int(m, 32);
    uint32_t bits = new_id(m);
    emitv(&m->functions, Op_Bitcast, 3, i32, bits, a);
    uint32_t low = new_id(m);
    emitv(&m->functions, Op_ShiftRightLogical, 4, i32, low, bits,
          const_u32(m, 16u));
    uint32_t carry = new_id(m);
    emitv(&m->functions, Op_BitwiseAnd, 4, i32, carry, low, const_u32(m, 1u));
    uint32_t bias = new_id(m);
    emitv(&m->functions, Op_IAdd, 4, i32, bias, carry, const_u32(m, 32767u));
    uint32_t rounded = new_id(m);
    emitv(&m->functions, Op_IAdd, 4, i32, rounded, bits, bias);
    uint32_t result = new_id(m);
    emitv(&m->functions, Op_ShiftRightLogical, 4, i32, result, rounded,
          const_u32(m, 16u));
    if (in->dest.name) store_name(fn, in->dest.name, result);
    return;
  }
  if (intrinsic == MTLC_INTRINSIC_GPU_PRMT_B32 && in->argument_count >= 3) {
    /* prmt gathers four bytes out of {b:a} by selector nibbles. Replayed as
     * four selects; the portable profile has no byte-permute instruction.
     * Only the plain (non-replicate) selector mode is modeled, which is the
     * mode the sign-extension variants are not. */
    uint32_t a = materialize(fn, &in->arguments[0], MTLC_TYPE_UINT32);
    uint32_t b = materialize(fn, &in->arguments[1], MTLC_TYPE_UINT32);
    uint32_t selector = materialize(fn, &in->arguments[2], MTLC_TYPE_UINT32);
    uint32_t i32 = type_int(m, 32);
    uint32_t result = const_u32(m, 0);
    for (unsigned lane = 0; lane < 4; lane++) {
      /* index = (selector >> (lane*4)) & 7; source = index < 4 ? a : b */
      uint32_t nibble_shift = const_u32(m, lane * 4u);
      uint32_t shifted_selector = new_id(m);
      emitv(&m->functions, Op_ShiftRightLogical, 4, i32, shifted_selector,
            selector, nibble_shift);
      uint32_t index = new_id(m);
      emitv(&m->functions, Op_BitwiseAnd, 4, i32, index, shifted_selector,
            const_u32(m, 7u));
      uint32_t from_high = new_id(m);
      emitv(&m->functions, Op_ShiftRightLogical, 4, i32, from_high, index,
            const_u32(m, 2u));
      uint32_t picks_b = new_id(m);
      emitv(&m->functions, Op_INotEqual, 4, type_bool(m), picks_b, from_high,
            const_u32(m, 0u));
      uint32_t source = new_id(m);
      emitv(&m->functions, Op_Select, 5, i32, source, picks_b, b, a);
      uint32_t byte_in_word = new_id(m);
      emitv(&m->functions, Op_BitwiseAnd, 4, i32, byte_in_word, index,
            const_u32(m, 3u));
      uint32_t byte_shift = new_id(m);
      emitv(&m->functions, Op_ShiftLeftLogical, 4, i32, byte_shift,
            byte_in_word, const_u32(m, 3u));
      uint32_t moved = new_id(m);
      emitv(&m->functions, Op_ShiftRightLogical, 4, i32, moved, source,
            byte_shift);
      uint32_t byte_value = new_id(m);
      emitv(&m->functions, Op_BitwiseAnd, 4, i32, byte_value, moved,
            const_u32(m, 0xFFu));
      uint32_t placed = new_id(m);
      emitv(&m->functions, Op_ShiftLeftLogical, 4, i32, placed, byte_value,
            const_u32(m, lane * 8u));
      uint32_t merged = new_id(m);
      emitv(&m->functions, Op_BitwiseOr, 4, i32, merged, result, placed);
      result = merged;
    }
    if (in->dest.name) store_name(fn, in->dest.name, result);
    return;
  }
  if ((intrinsic == MTLC_INTRINSIC_GPU_LOAD4_F32 ||
       intrinsic == MTLC_INTRINSIC_GPU_LOAD4_U32 ||
       intrinsic == MTLC_INTRINSIC_GPU_STORE4_F32 ||
       intrinsic == MTLC_INTRINSIC_GPU_STORE4_U32) &&
      in->argument_count >= 2) {
    /* The portable profile moves the same four elements as four scalar
     * accesses: same values, same order, without the 128-bit transaction PTX
     * gets. Vectorizing this needs a device profile with vector loads. */
    int is_load = intrinsic == MTLC_INTRINSIC_GPU_LOAD4_F32 ||
                  intrinsic == MTLC_INTRINSIC_GPU_LOAD4_U32;
    MtlcTypeKind elem = (intrinsic == MTLC_INTRINSIC_GPU_LOAD4_F32 ||
                         intrinsic == MTLC_INTRINSIC_GPU_STORE4_F32)
                            ? MTLC_TYPE_FLOAT32
                            : MTLC_TYPE_UINT32;
    SpvDesc vector_desc = operand_desc(fn, &in->arguments[0]);
    SpvDesc scalar_desc = operand_desc(fn, &in->arguments[1]);
    uint32_t vector_base = materialize(fn, &in->arguments[0], MTLC_TYPE_UINT64);
    uint32_t scalar_base = materialize(fn, &in->arguments[1], MTLC_TYPE_UINT64);
    uint32_t u64 = type_int(m, 64);
    uint32_t element_type = kind_type(m, elem);
    for (unsigned lane = 0; lane < 4; lane++) {
      uint32_t vector_address = vector_base;
      uint32_t scalar_address = scalar_base;
      if (lane != 0) {
        uint32_t offset =
            const_scalar_int(m, MTLC_TYPE_UINT64, (long long)(lane * 4u));
        vector_address = new_id(m);
        scalar_address = new_id(m);
        emitv(&m->functions, Op_IAdd, 4, u64, vector_address, vector_base,
              offset);
        emitv(&m->functions, Op_IAdd, 4, u64, scalar_address, scalar_base,
              offset);
      }
      uint32_t vector_pointer = typed_memory_ptr(fn, vector_address, elem,
                                                 vector_desc.address_space);
      uint32_t scalar_pointer = typed_memory_ptr(fn, scalar_address, elem,
                                                 scalar_desc.address_space);
      uint32_t value = new_id(m);
      if (is_load) {
        emitv(&m->functions, Op_Load, 5, element_type, value, vector_pointer,
              (unsigned)MemAccess_Aligned, 4u);
        emitv(&m->functions, Op_Store, 4, scalar_pointer, value,
              (unsigned)MemAccess_Aligned, 4u);
      } else {
        emitv(&m->functions, Op_Load, 5, element_type, value, scalar_pointer,
              (unsigned)MemAccess_Aligned, 4u);
        emitv(&m->functions, Op_Store, 4, vector_pointer, value,
              (unsigned)MemAccess_Aligned, 4u);
      }
    }
    return;
  }
  if (is_math_intrinsic(intrinsic) && in->argument_count >= 1) {
    uint32_t a = materialize(fn, &in->arguments[0], MTLC_TYPE_FLOAT32);
    uint32_t f32 = type_float(m, 32);
    int num;
    if (intrinsic == MTLC_INTRINSIC_GPU_SQRT_F32) num = CL_sqrt;
    else if (intrinsic == MTLC_INTRINSIC_GPU_RSQRT_F32) num = CL_rsqrt;
    else if (intrinsic == MTLC_INTRINSIC_GPU_ABS_F32) num = CL_fabs;
    else if (intrinsic == MTLC_INTRINSIC_GPU_SIN_F32) num = CL_sin;
    else if (intrinsic == MTLC_INTRINSIC_GPU_COS_F32) num = CL_cos;
    else if (intrinsic == MTLC_INTRINSIC_GPU_LOG_F32) num = CL_log;
    else num = CL_exp;
    uint32_t r = new_id(m);
    emitv(&m->functions, Op_ExtInst, 5, f32, r, m->opencl_ext, (unsigned)num, a);
    if (in->dest.name) store_name(fn, in->dest.name, r);
    return;
  }
  if (is_atomic_intrinsic(intrinsic) &&
      in->argument_count >= (size_t)ir_intrinsic_arity(intrinsic)) {
    int is64 =
        ir_intrinsic_atomic_value_kind(intrinsic) == MTLC_TYPE_UINT64;
    int is_cas = ir_intrinsic_is_compare_exchange(intrinsic);
    int is_load = ir_intrinsic_is_atomic_load(intrinsic);
    int is_store = ir_intrinsic_is_atomic_store(intrinsic);
    MtlcTypeKind vk = is64 ? MTLC_TYPE_UINT64 : MTLC_TYPE_UINT32;
    int elemsz = is64 ? 8 : 4;
    uint32_t buf = materialize(fn, &in->arguments[0], MTLC_TYPE_POINTER);
    uint32_t idx = materialize(fn, &in->arguments[1], MTLC_TYPE_INT64);
    uint32_t val =
        is_load ? 0 : materialize(fn, &in->arguments[2], vk);
    uint32_t u64 = type_int(m, 64);
    uint32_t off = new_id(m);
    emitv(&m->functions, Op_IMul, 4, u64, off, idx,
          const_scalar_int(m, MTLC_TYPE_INT64, elemsz));
    uint32_t addr = new_id(m);
    emitv(&m->functions, Op_IAdd, 4, u64, addr, buf, off);
    int scope_value = spv_atomic_scope(in->memory_scope);
    int semantics_value =
        spv_atomic_semantics(in->memory_order, in->address_space);
    int failure_semantics_value =
        is_cas ? spv_atomic_semantics(in->failure_memory_order,
                                      in->address_space)
               : 0;
    if (scope_value < 0 || semantics_value < 0 ||
        (is_load && in->memory_order != MTLC_MEMORY_ORDER_RELAXED &&
         in->memory_order != MTLC_MEMORY_ORDER_ACQUIRE &&
         in->memory_order != MTLC_MEMORY_ORDER_SEQ_CST) ||
        (is_store && in->memory_order != MTLC_MEMORY_ORDER_RELAXED &&
         in->memory_order != MTLC_MEMORY_ORDER_RELEASE &&
         in->memory_order != MTLC_MEMORY_ORDER_SEQ_CST) ||
        (is_cas && (failure_semantics_value < 0 ||
                    !spv_compare_exchange_failure_valid(
                        in->memory_order, in->failure_memory_order))) ||
        in->address_space == MTLC_ADDRESS_SPACE_CONSTANT ||
        in->address_space == MTLC_ADDRESS_SPACE_PRIVATE ||
        (in->address_space == MTLC_ADDRESS_SPACE_WORKGROUP &&
         in->memory_scope > MTLC_MEMORY_SCOPE_WORKGROUP)) {
      mod_error(m,
                "SPIR-V: invalid atomic memory contract (space=%d success=%d failure=%d scope=%d)",
                (int)in->address_space, (int)in->memory_order,
                (int)in->failure_memory_order, (int)in->memory_scope);
      return;
    }
    uint32_t p = typed_memory_ptr(fn, addr, vk, in->address_space);
    uint32_t vt = kind_type(m, vk);
    uint32_t sc = const_u32(m, scope_value);
    uint32_t sem = const_u32(m, semantics_value);
    uint32_t r = 0;
    if (is_load) {
      r = new_id(m);
      emitv(&m->functions, Op_AtomicLoad, 5, vt, r, p, sc, sem);
    } else if (is_store) {
      emitv(&m->functions, Op_AtomicStore, 4, p, sc, sem, val);
    } else if (is_cas) {
      r = new_id(m);
      uint32_t desired = materialize(fn, &in->arguments[3], vk);
      uint32_t failure_sem = const_u32(m, failure_semantics_value);
      /* SPIR-V orders these operands as desired value, then comparator. */
      emitv(&m->functions, Op_AtomicCompareExchange, 8, vt, r, p, sc, sem,
            failure_sem, desired, val);
    } else {
      r = new_id(m);
      unsigned opcode =
          intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_ADD_U32 ||
                  intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_ADD_U64
              ? Op_AtomicIAdd
          : intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_SUB_U32 ||
                  intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_SUB_U64
              ? Op_AtomicISub
          : intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_MIN_U32 ||
                  intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_MIN_U64
              ? Op_AtomicUMin
          : intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_MAX_U32 ||
                  intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_MAX_U64
              ? Op_AtomicUMax
          : intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_AND_U32 ||
                  intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_AND_U64
              ? Op_AtomicAnd
          : intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_OR_U32 ||
                  intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_OR_U64
              ? Op_AtomicOr
          : intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_XOR_U32 ||
                  intrinsic == MTLC_INTRINSIC_GPU_ATOMIC_XOR_U64
              ? Op_AtomicXor
              : Op_AtomicExchange;
      emitv(&m->functions, opcode, 6, vt, r, p, sc, sem, val);
    }
    if (is64) m->use_atomics64 = 1;
    if (in->dest.name && !is_store) store_name(fn, in->dest.name, r);
    return;
  }
  if (intrinsic == MTLC_INTRINSIC_NONE) {
    long callee_index = spv_function_index(m->program, callee);
    const IRModuleSymbol *callee_symbol =
        ir_program_lookup_symbol(m->program, callee);
    if (callee_index < 0 || !callee_symbol ||
        callee_symbol->kind != IR_MODSYM_FUNCTION ||
        !m->device_function_ids[(size_t)callee_index]) {
      mod_error(m, "SPIR-V: device call target '%s' has no definition",
                callee ? callee : "?");
      return;
    }
    IRFunction *callee_function =
        m->program->functions[(size_t)callee_index];
    if (in->argument_count != callee_function->parameter_count ||
        in->argument_count != callee_symbol->param_count) {
      mod_error(m,
                "SPIR-V: device call '%s' expects %zu arguments, received %zu",
                callee, callee_symbol->param_count, in->argument_count);
      return;
    }
    const MtlcType *return_type = spv_function_return_type(
        m->program, callee_function, callee_symbol);
    int returns_void =
        spv_type_is_void(return_type, callee_function->return_type_name);
    SpvDesc return_desc = returns_void
                              ? (SpvDesc){.kind = MTLC_TYPE_VOID}
                              : (return_type
                                     ? desc_from_type(return_type)
                                     : desc_from_typename(
                                           callee_function->return_type_name));
    uint32_t return_type_id =
        returns_void ? type_void(m) : kind_type(m, return_desc.kind);
    uint32_t result_id = new_id(m);
    Wb call = {0};
    wb_push(&call, return_type_id);
    wb_push(&call, result_id);
    wb_push(&call, m->device_function_ids[(size_t)callee_index]);
    for (size_t a = 0; a < in->argument_count && !m->error; a++) {
      SpvDesc argument_desc = desc_from_type(callee_symbol->param_types[a]);
      wb_push(&call,
              materialize(fn, &in->arguments[a], argument_desc.kind));
    }
    if (!m->error) {
      emit_ops(&m->functions, Op_FunctionCall, &call);
      fn->builtin_mask |=
          m->function_builtin_masks[(size_t)callee_index];
      if (returns_void) {
        if (in->dest.name) {
          mod_error(m, "SPIR-V: void device call '%s' has a result", callee);
        }
      } else if (in->dest.name) {
        store_name(fn, in->dest.name, result_id);
      }
    }
    wb_free(&call);
    return;
  }
  mod_error(m, "SPIR-V: unsupported call '%s'", callee ? callee : "?");
}

/* emit one non-terminator body instruction */
static void emit_body_instr(SpvFn *fn, const IRInstruction *in) {
  if (fn->m->error) return;
  switch (in->op) {
  case IR_OP_NOP:
  case IR_OP_LABEL:
  case IR_OP_ADDRESS_SPACE_ALLOC:
  case IR_OP_DECLARE_LOCAL:
    break; /* declarations handled in the pre-pass */
  case IR_OP_BARRIER:
    if (in->memory_scope != MTLC_MEMORY_SCOPE_WORKGROUP) {
      mod_error(fn->m, "SPIR-V: unsupported execution scope on barrier");
      break;
    }
    emit_workgroup_barrier(fn, in->memory_order, in->memory_regions);
    break;
  case IR_OP_ASYNC_COPY:
    /* OpenCL 2.0 has no enabled device-side async-copy capability in this
     * profile. Replay synchronously; commit/wait below are consequently
     * completion no-ops with identical observable semantics. */
    emit_async_copy(fn, in);
    break;
  case IR_OP_ASYNC_COMMIT:
  case IR_OP_ASYNC_WAIT:
    break;
  case IR_OP_TENSOR_MMA:
    mod_error(fn->m,
              "SPIR-V OpenCL 2.0 profile has no cooperative-matrix capability; tensor MMA requires a newer explicit device profile");
    break;
  case IR_OP_TENSOR_MATMUL:
    mod_error(fn->m,
              "SPIR-V OpenCL 2.0 profile has no exact bounded matrix-region lowering; tensor_matmul requires explicit cooperative-matrix and exact edge support");
    break;
  case IR_OP_TENSOR_EPILOGUE:
    mod_error(fn->m,
              "SPIR-V OpenCL 2.0 profile has no exact cooperative tensor-epilogue lowering; select a profile with ordered collective replay or native tensor epilogues");
    break;
  case IR_OP_TENSOR_TRANSFER:
    mod_error(fn->m,
              "SPIR-V OpenCL 2.0 profile has no multidimensional workgroup-transfer lowering; select a backend profile with exact tensor-transfer or cooperative-replay support");
    break;
  case IR_OP_TENSOR_COMMIT:
    /* The current profile rejects the corresponding tensor operations above.
     * A future cooperative-matrix backend may replay each MMA and keep this
     * neutral residency marker as a no-op. */
    break;
  case IR_OP_ASSIGN: {
    /* store lhs into dest, coerced to dest's declared kind */
    SpvBind *db = find_bind(fn, in->dest.name);
    MtlcTypeKind dk = db ? db->d.kind : operand_desc(fn, &in->lhs).kind;
    uint32_t v = materialize(fn, &in->lhs, dk);
    if (in->dest.name) store_name(fn, in->dest.name, v);
    break;
  }
  case IR_OP_BINARY:
    emit_binary(fn, in);
    break;
  case IR_OP_UNARY:
    emit_unary(fn, in);
    break;
  case IR_OP_LOAD:
    emit_load(fn, in);
    break;
  case IR_OP_STORE:
    emit_store(fn, in);
    break;
  case IR_OP_CAST:
    emit_cast(fn, in);
    break;
  case IR_OP_CALL:
    emit_call(fn, in);
    break;
  case IR_OP_ADDRESS_OF:
    mod_error(fn->m,
              "SPIR-V: address-of (&local) not supported in device functions yet");
    break;
  default:
    mod_error(fn->m, "SPIR-V: unsupported IR opcode %d in device function",
              in->op);
    break;
  }
}

/* ============================ CFG emission ============================ */
typedef struct {
  size_t lo, hi;             /* instruction range [lo, hi) */
  const char *label;         /* leading label name, or NULL */
  const IRInstruction *term; /* terminator instruction, or NULL (fallthrough) */
} SpvBlock;

static int is_terminator(IROpcode op) {
  return op == IR_OP_JUMP || op == IR_OP_BRANCH_ZERO || op == IR_OP_BRANCH_EQ ||
         op == IR_OP_RETURN;
}

/* find the block index whose leading label matches `name` */
static long block_of_label(SpvBlock *blocks, size_t n, const char *name) {
  if (!name) return -1;
  for (size_t i = 0; i < n; i++) {
    if (blocks[i].label && strcmp(blocks[i].label, name) == 0) {
      return (long)i;
    }
  }
  return -1;
}

/* compute the branch condition of a conditional block as a scalar bool id.
 * BRANCH_ZERO: (cond != 0). BRANCH_EQ: (lhs == rhs). */
static uint32_t branch_cond_bool(SpvFn *fn, const IRInstruction *term) {
  SpvMod *m = fn->m;
  uint32_t bt = type_bool(m);
  uint32_t cond = new_id(m);
  if (term->op == IR_OP_BRANCH_ZERO) {
    SpvDesc d = operand_desc(fn, &term->lhs);
    if (kind_is_float(d.kind)) {
      uint32_t v = materialize(fn, &term->lhs, d.kind);
      uint32_t z = const_float(m, d.kind, 0.0);
      emitv(&m->functions, Op_FUnordNotEqual, 4, bt, cond, v, z);
    } else {
      MtlcTypeKind k = kind_bits(d.kind) == 64 ? MTLC_TYPE_INT64 : MTLC_TYPE_INT32;
      uint32_t v = materialize(fn, &term->lhs, k);
      uint32_t z = const_scalar_int(m, k, 0);
      emitv(&m->functions, Op_INotEqual, 4, bt, cond, v, z);
    }
  } else { /* BRANCH_EQ */
    SpvDesc la = operand_desc(fn, &term->lhs), ra = operand_desc(fn, &term->rhs);
    int is_float = kind_is_float(la.kind) || kind_is_float(ra.kind);
    MtlcTypeKind k =
        is_float ? ((la.kind == MTLC_TYPE_FLOAT64 || ra.kind == MTLC_TYPE_FLOAT64)
                        ? MTLC_TYPE_FLOAT64
                        : MTLC_TYPE_FLOAT32)
                 : (kind_bits(la.kind) == 64 || kind_bits(ra.kind) == 64
                        ? MTLC_TYPE_INT64
                        : MTLC_TYPE_INT32);
    uint32_t a = materialize(fn, &term->lhs, k);
    uint32_t b = materialize(fn, &term->rhs, k);
    emitv(&m->functions, is_float ? Op_FOrdEqual : Op_IEqual, 4, bt, cond, a, b);
  }
  return cond;
}

/* ---- pre-pass: register a descriptor for every named value ---- */
static void register_values(SpvFn *fn, IRFunction *func) {
  const IRModuleSymbol *function_symbol =
      fn->m->program ? ir_program_lookup_symbol(fn->m->program, func->name) : NULL;
  for (size_t p = 0; p < func->parameter_count; p++) {
    if (!func->parameter_names || !func->parameter_names[p]) continue;
    const char *tn = func->parameter_types ? func->parameter_types[p] : NULL;
    const MtlcType *pt = function_symbol &&
                                 function_symbol->kind == IR_MODSYM_FUNCTION &&
                                 p < function_symbol->param_count
                             ? function_symbol->param_types[p]
                             : NULL;
    add_bind(fn, func->parameter_names[p],
             pt ? desc_from_type(pt) : desc_from_typename(tn));
  }
  for (size_t i = 0; i < func->instruction_count; i++) {
    const IRInstruction *in = &func->instructions[i];
    if (in->op == IR_OP_DECLARE_LOCAL ||
        in->op == IR_OP_ADDRESS_SPACE_ALLOC) {
      if (in->dest.name && !find_bind(fn, in->dest.name)) {
        add_bind(fn, in->dest.name,
                 in->value_type ? desc_from_type(in->value_type)
                                : desc_from_typename(in->text));
      }
      continue;
    }
    if (!in->dest.name) continue;
    switch (in->op) {
    case IR_OP_BINARY:
    case IR_OP_UNARY:
    case IR_OP_LOAD:
    case IR_OP_ASSIGN:
    case IR_OP_CAST:
    case IR_OP_CALL:
      if (!find_bind(fn, in->dest.name)) {
        add_bind(fn, in->dest.name, result_desc(fn, in));
      }
      break;
    default:
      break;
    }
  }
}

/* Emit one reachable GPU function. Kernels retain the OpenCL entry ABI;
 * ordinary reachable functions use the same neutral scalar IR ABI internally. */
static uint32_t emit_device_function(SpvMod *m, IRFunction *func,
                                     size_t function_index) {
  SpvFn fn = {0};
  fn.m = m;
  fn.function = func;
  fn.function_index = function_index;
  const IRModuleSymbol *function_symbol =
      m->program ? ir_program_lookup_symbol(m->program, func->name) : NULL;
  const MtlcType *return_type =
      spv_function_return_type(m->program, func, function_symbol);
  fn.returns_void =
      spv_type_is_void(return_type, func ? func->return_type_name : NULL);
  fn.return_desc =
      fn.returns_void
          ? (SpvDesc){.kind = MTLC_TYPE_VOID}
          : (return_type ? desc_from_type(return_type)
                         : desc_from_typename(func->return_type_name));
  if (func->is_kernel && !fn.returns_void) {
    mod_error(m, "SPIR-V: kernel '%s' must return void",
              func->name ? func->name : "?");
    return 0;
  }

  /* ---- build basic blocks from the label/branch stream ---- */
  size_t count = func->instruction_count;
  size_t *starts = calloc(count + 1, sizeof(size_t));
  size_t nstarts = 0;
  starts[nstarts++] = 0;
  for (size_t i = 0; i < count; i++) {
    IROpcode op = func->instructions[i].op;
    if (op == IR_OP_LABEL && (nstarts == 0 || starts[nstarts - 1] != i)) {
      starts[nstarts++] = i;
    }
    if (is_terminator(op) && i + 1 < count &&
        (nstarts == 0 || starts[nstarts - 1] != i + 1)) {
      starts[nstarts++] = i + 1;
    }
  }
  SpvBlock *blocks = calloc(nstarts ? nstarts : 1, sizeof(SpvBlock));
  size_t nblocks = 0;
  for (size_t k = 0; k < nstarts; k++) {
    size_t lo = starts[k];
    size_t hi = (k + 1 < nstarts) ? starts[k + 1] : count;
    if (lo >= hi) continue;
    SpvBlock *bl = &blocks[nblocks++];
    bl->lo = lo;
    bl->hi = hi;
    bl->label = (func->instructions[lo].op == IR_OP_LABEL)
                    ? func->instructions[lo].text
                    : NULL;
    bl->term = is_terminator(func->instructions[hi - 1].op)
                   ? &func->instructions[hi - 1]
                   : NULL;
  }

  register_values(&fn, func);

  /* OpenCL represents launch-sized local memory as a Workgroup pointer kernel
   * argument. Select the most strictly aligned dynamic view as the hidden ABI
   * pointee type; every view is then materialized from that same pointer-sized
   * value, preserving the IR's intentional aliasing contract. */
  const IRInstruction *dynamic_workgroup_abi_view = NULL;
  size_t dynamic_workgroup_alignment = 0;
  for (size_t i = 0; i < func->instruction_count && !m->error; i++) {
    const IRInstruction *in = &func->instructions[i];
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
      mod_error(m, "SPIR-V: invalid dynamic workgroup view in '%s'",
                func->name ? func->name : "?");
      break;
    }
    size_t alignment = mtlc_type_alignment(in->value_type->base_type);
    if (!dynamic_workgroup_abi_view ||
        alignment > dynamic_workgroup_alignment) {
      dynamic_workgroup_abi_view = in;
      dynamic_workgroup_alignment = alignment;
    }
  }

  /* ---- ids: entry, one label per IR block, a shared fall-off-the-end exit ---- */
  uint32_t func_id = m->device_function_ids[function_index];
  uint32_t entry_id = new_id(m);
  uint32_t exit_id = new_id(m);
  int exit_used = 0;
  uint32_t *block_id = calloc(nblocks ? nblocks : 1, sizeof(uint32_t));
  for (size_t i = 0; i < nblocks; i++) block_id[i] = new_id(m);

  /* ---- parameter + function types ---- */
  uint32_t voidt = type_void(m);
  uint32_t function_return_type =
      fn.returns_void ? voidt : kind_type(m, fn.return_desc.kind);
  size_t total_parameter_count =
      func->parameter_count + (dynamic_workgroup_abi_view ? 1u : 0u);
  size_t dynamic_workgroup_parameter = func->parameter_count;
  uint32_t *ptypes = calloc(total_parameter_count + 1, sizeof(uint32_t));
  uint32_t *pids = calloc(total_parameter_count + 1, sizeof(uint32_t));
  SpvDesc *pdesc = calloc(total_parameter_count + 1, sizeof(SpvDesc));
  for (size_t p = 0; p < func->parameter_count; p++) {
    const char *tn = func->parameter_types ? func->parameter_types[p] : NULL;
    const MtlcType *pt = function_symbol &&
                                 function_symbol->kind == IR_MODSYM_FUNCTION &&
                                 p < function_symbol->param_count
                             ? function_symbol->param_types[p]
                             : NULL;
    SpvDesc d = pt ? desc_from_type(pt) : desc_from_typename(tn);
    pdesc[p] = d;
    if (func->is_kernel && d.is_ptr) {
      int sc = spv_storage_class(d.address_space);
      if (sc < 0 || d.address_space == MTLC_ADDRESS_SPACE_PRIVATE ||
          d.address_space == MTLC_ADDRESS_SPACE_CONSTANT) {
        mod_error(m,
                  "SPIR-V OpenCL 2.0: kernel parameter %zu has unsupported address space %d",
                  p, (int)d.address_space);
        sc = SC_CrossWorkgroup;
      }
      ptypes[p] = type_pointer(m, sc, kind_type(m, d.elem));
    } else {
      ptypes[p] = kind_type(m, d.kind);
    }
    pids[p] = new_id(m);
  }
  if (dynamic_workgroup_abi_view) {
    SpvDesc dynamic_desc =
        desc_from_type(dynamic_workgroup_abi_view->value_type);
    ptypes[dynamic_workgroup_parameter] =
        type_pointer(m, SC_Workgroup, kind_type(m, dynamic_desc.elem));
    pids[dynamic_workgroup_parameter] = new_id(m);
  }
  Wb ftops = {0};
  wb_push(&ftops, function_return_type);
  for (size_t p = 0; p < total_parameter_count; p++)
    wb_push(&ftops, ptypes[p]);
  /* intern the function type by structural key */
  char ftkey[256];
  int kn = snprintf(ftkey, sizeof(ftkey), "fn:%u", function_return_type);
  for (size_t p = 0;
       p < total_parameter_count && kn < (int)sizeof(ftkey) - 12; p++) {
    kn += snprintf(ftkey + kn, sizeof(ftkey) - (size_t)kn, ":%u", ptypes[p]);
  }
  uint32_t functype = cache_get(m, ftkey);
  if (!functype) {
    functype = new_id(m);
    Wb ft = {0};
    wb_push(&ft, functype);
    wb_push(&ft, function_return_type);
    for (size_t p = 0; p < total_parameter_count; p++)
      wb_push(&ft, ptypes[p]);
    emit_ops(&m->typesconsts, Op_TypeFunction, &ft);
    wb_free(&ft);
    cache_put(m, ftkey, functype);
  }
  wb_free(&ftops);

  /* ---- OpFunction + parameters ---- */
  emitv(&m->functions, Op_Function, 4, function_return_type, func_id, 0u,
        functype);
  for (size_t p = 0; p < total_parameter_count; p++) {
    emitv(&m->functions, Op_FunctionParameter, 2, ptypes[p], pids[p]);
  }

  /* ---- entry block: declare variables, shadow params ---- */
  emitv(&m->functions, Op_Label, 1, entry_id);
  for (size_t i = 0; i < fn.nbinds; i++) {
    SpvBind *b = &fn.binds[i];
    uint32_t vt = kind_type(m, b->d.kind);
    uint32_t pt = type_pointer(m, SC_Function, vt);
    b->var_id = new_id(m);
    emitv(&m->functions, Op_Variable, 3, pt, b->var_id, (unsigned)SC_Function);
  }
  /* Declare every static allocation before emitting entry-block executable
   * instructions. Workgroup variables live at module scope; private variables
   * use Function storage and must be the first instructions in the entry
   * block. Keep their ids by IR instruction so the materialization pass below
   * can convert them into the neutral pointer-sized value model. */
  uint32_t *allocation_variables =
      calloc(func->instruction_count ? func->instruction_count : 1,
             sizeof(uint32_t));
  for (size_t i = 0; i < func->instruction_count && !m->error; i++) {
    const IRInstruction *in = &func->instructions[i];
    if (in->op != IR_OP_ADDRESS_SPACE_ALLOC) continue;
    int is_dynamic =
        in->rhs.kind == IR_OPERAND_INT && in->rhs.int_value == 0;
    if (!func->is_kernel || !in->dest.name || !in->value_type ||
        in->value_type->kind != MTLC_TYPE_POINTER ||
        !in->value_type->base_type || in->rhs.kind != IR_OPERAND_INT ||
        in->rhs.int_value < 0 || in->rhs.int_value > UINT32_MAX ||
        (in->address_space != MTLC_ADDRESS_SPACE_WORKGROUP &&
         in->address_space != MTLC_ADDRESS_SPACE_PRIVATE) ||
        in->value_type->address_space != in->address_space ||
        (is_dynamic && in->address_space != MTLC_ADDRESS_SPACE_WORKGROUP)) {
      mod_error(m, "SPIR-V: invalid address-space allocation in '%s'",
                func->name ? func->name : "?");
      break;
    }
    SpvBind *binding = find_bind(&fn, in->dest.name);
    SpvDesc descriptor = desc_from_type(in->value_type);
    int storage_class = spv_storage_class(in->address_space);
    if (!binding || !descriptor.is_ptr || storage_class < 0 ||
        mtlc_type_size(in->value_type->base_type) == 0) {
      mod_error(m, "SPIR-V: allocation '%s' has an unsupported element type",
                in->dest.name);
      break;
    }
    if (is_dynamic) continue;
    uint32_t element_type = kind_type(m, descriptor.elem);
    uint32_t array_type =
        type_array(m, element_type, (uint32_t)in->rhs.int_value);
    uint32_t pointer_type = type_pointer(m, storage_class, array_type);
    uint32_t variable = new_id(m);
    Wb *variable_section = storage_class == SC_Function ? &m->functions
                                                        : &m->typesconsts;
    emitv(variable_section, Op_Variable, 3, pointer_type, variable,
          (unsigned)storage_class);
    allocation_variables[i] = variable;
  }
  /* Materialize neutral allocations. Dynamic workgroup views all receive the
   * same hidden Workgroup parameter. Pointers remain 64-bit values in the IR
   * value model, just like explicit kernel pointer parameters; typed accesses
   * convert them back with the exact storage class in their descriptor. */
  for (size_t i = 0; i < func->instruction_count && !m->error; i++) {
    const IRInstruction *in = &func->instructions[i];
    if (in->op != IR_OP_ADDRESS_SPACE_ALLOC) continue;
    SpvBind *binding = find_bind(&fn, in->dest.name);
    int is_dynamic = in->rhs.int_value == 0;
    if (is_dynamic) {
      if (!dynamic_workgroup_abi_view ||
          dynamic_workgroup_parameter >= total_parameter_count) {
        mod_error(m, "SPIR-V: dynamic workgroup ABI was not materialized");
        break;
      }
      uint32_t as_integer = new_id(m);
      emitv(&m->functions, Op_ConvertPtrToU, 3, type_int(m, 64),
            as_integer, pids[dynamic_workgroup_parameter]);
      emitv(&m->functions, Op_Store, 2, binding->var_id, as_integer);
      continue;
    }
    uint32_t variable = allocation_variables[i];
    if (!variable) {
      mod_error(m, "SPIR-V: allocation '%s' was not declared", in->dest.name);
      break;
    }
    uint32_t as_integer = new_id(m);
    emitv(&m->functions, Op_ConvertPtrToU, 3, type_int(m, 64), as_integer,
          variable);
    emitv(&m->functions, Op_Store, 2, binding->var_id, as_integer);
  }
  free(allocation_variables);
  /* store incoming parameters into their shadow variables */
  for (size_t p = 0; p < func->parameter_count; p++) {
    if (!func->parameter_names || !func->parameter_names[p]) continue;
    SpvBind *b = find_bind(&fn, func->parameter_names[p]);
    if (!b) continue;
    if (func->is_kernel && pdesc[p].is_ptr) {
      uint32_t u64 = type_int(m, 64);
      uint32_t asint = new_id(m);
      emitv(&m->functions, Op_ConvertPtrToU, 3, u64, asint, pids[p]);
      emitv(&m->functions, Op_Store, 2, b->var_id, asint);
    } else {
      emitv(&m->functions, Op_Store, 2, b->var_id, pids[p]);
    }
  }
  if (nblocks > 0) {
    emitv(&m->functions, Op_Branch, 1, block_id[0]);
  } else if (fn.returns_void) {
    emitv(&m->functions, Op_Return, 0);
  } else {
    mod_error(m, "SPIR-V: non-void device function '%s' has no return",
              func->name ? func->name : "?");
  }

  /* ---- one SPIR-V block per IR block, branches mapped directly ---- */
  for (size_t i = 0; i < nblocks && !m->error; i++) {
    SpvBlock *bl = &blocks[i];
    emitv(&m->functions, Op_Label, 1, block_id[i]);
    for (size_t j = bl->lo; j < bl->hi && !m->error; j++) {
      const IRInstruction *in = &func->instructions[j];
      if (in == bl->term) break; /* terminator handled below */
      emit_body_instr(&fn, in);
    }
    const IRInstruction *term = bl->term;
    if (term && term->op == IR_OP_RETURN) {
      if (fn.returns_void) {
        if (term->lhs.kind != IR_OPERAND_NONE) {
          mod_error(m, "SPIR-V: void device function '%s' returns a value",
                    func->name ? func->name : "?");
          break;
        }
        emitv(&m->functions, Op_Return, 0);
      } else if (term->lhs.kind == IR_OPERAND_NONE) {
        mod_error(m,
                  "SPIR-V: non-void device function '%s' has an empty return",
                  func->name ? func->name : "?");
        break;
      } else {
        uint32_t value = materialize(&fn, &term->lhs, fn.return_desc.kind);
        emitv(&m->functions, Op_ReturnValue, 1, value);
      }
    } else if (term && term->op == IR_OP_JUMP) {
      long t = block_of_label(blocks, nblocks, term->text);
      if (t < 0) {
        mod_error(m, "SPIR-V: jump to unknown label '%s'",
                  term->text ? term->text : "?");
        break;
      }
      emitv(&m->functions, Op_Branch, 1, block_id[t]);
    } else if (term &&
               (term->op == IR_OP_BRANCH_ZERO || term->op == IR_OP_BRANCH_EQ)) {
      long taken = block_of_label(blocks, nblocks, term->text);
      if (taken < 0) {
        mod_error(m, "SPIR-V: branch to unknown label '%s'",
                  term->text ? term->text : "?");
        break;
      }
      uint32_t fall_id;
      if (i + 1 < nblocks) {
        fall_id = block_id[i + 1];
      } else {
        if (!fn.returns_void) {
          mod_error(m,
                    "SPIR-V: non-void device function '%s' can fall through",
                    func->name ? func->name : "?");
          break;
        }
        fall_id = exit_id;
        exit_used = 1;
      }
      uint32_t taken_id = block_id[taken];
      uint32_t cond = branch_cond_bool(&fn, term);
      /* BRANCH_ZERO's condition is (value != 0): take the target when FALSE.
       * BRANCH_EQ's condition is (lhs == rhs):   take the target when TRUE. */
      uint32_t t_true = (term->op == IR_OP_BRANCH_ZERO) ? fall_id : taken_id;
      uint32_t t_false = (term->op == IR_OP_BRANCH_ZERO) ? taken_id : fall_id;
      emitv(&m->functions, Op_BranchConditional, 3, cond, t_true, t_false);
    } else {
      /* fallthrough: continue into the next block, or return at the end */
      if (i + 1 < nblocks) {
        emitv(&m->functions, Op_Branch, 1, block_id[i + 1]);
      } else if (fn.returns_void) {
        emitv(&m->functions, Op_Return, 0);
      } else {
        mod_error(m,
                  "SPIR-V: non-void device function '%s' can fall through",
                  func->name ? func->name : "?");
        break;
      }
    }
  }

  if (exit_used) {
    emitv(&m->functions, Op_Label, 1, exit_id);
    emitv(&m->functions, Op_Return, 0);
  }
  emitv(&m->functions, Op_FunctionEnd, 0);

  m->function_builtin_masks[function_index] = fn.builtin_mask;

  /* ---- entry point (with the BuiltIn Input vars this kernel touched) ---- */
  if (!m->error && func->is_kernel) {
    Wb ep = {0};
    wb_push(&ep, (uint32_t)ExecModel_Kernel);
    wb_push(&ep, func_id);
    wb_str(&ep, func->name ? func->name : "kernel");
    static const int gpu_builtins[] = {
        BI_NumWorkgroups, BI_WorkgroupSize, BI_WorkgroupId,
        BI_LocalInvocationId, BI_SubgroupSize,
        BI_SubgroupLocalInvocationId};
    for (size_t i = 0; i < sizeof(gpu_builtins) / sizeof(gpu_builtins[0]); i++) {
      int builtin = gpu_builtins[i];
      if (fn.builtin_mask & (UINT64_C(1) << builtin)) {
        wb_push(&ep, builtin_var(m, builtin));
      }
    }
    emit_ops(&m->entrypoints, Op_EntryPoint, &ep);
    wb_free(&ep);
    if (func->kernel_block[0] > 0) {
      /* `kernel(block = ...)`: the required launch shape as the LocalSize
       * (reqd_work_group_size) execution mode, so an OpenCL consumer rejects
       * a mismatched enqueue. */
      emitv(&m->execmodes, Op_ExecutionMode, 5, func_id,
            17u /* LocalSize */, (uint32_t)func->kernel_block[0],
            (uint32_t)(func->kernel_block[1] > 0 ? func->kernel_block[1] : 1),
            (uint32_t)(func->kernel_block[2] > 0 ? func->kernel_block[2] : 1));
    }
  }

  for (size_t i = 0; i < fn.nbinds; i++) free(fn.binds[i].name);
  free(fn.binds);
  free(starts);
  free(blocks);
  free(block_id);
  free(ptypes);
  free(pids);
  free(pdesc);
  return m->error ? 0 : func_id;
}

/* ============================ driver ============================ */
static void write_word_le(FILE *out, uint32_t w) {
  fputc((int)(w & 0xff), out);
  fputc((int)((w >> 8) & 0xff), out);
  fputc((int)((w >> 16) & 0xff), out);
  fputc((int)((w >> 24) & 0xff), out);
}
static void write_section(FILE *out, const Wb *w) {
  for (size_t i = 0; i < w->len; i++) write_word_le(out, w->data[i]);
}

int spirv_emit_program(IRProgram *program, CodeGenerator *generator, FILE *out,
                       char **error) {
  (void)generator;
  if (error) *error = NULL;
  if (!program || !out) {
    if (error) *error = strdup("spirv_emit_program: null program/out");
    return 0;
  }

  IRGpuCallGraph graph = {0};
  char *graph_error = NULL;
  if (!ir_program_build_gpu_call_graph(program, &graph, &graph_error)) {
    if (error) {
      *error = graph_error ? graph_error
                           : strdup("SPIR-V: invalid GPU call graph");
    } else {
      free(graph_error);
    }
    return 0;
  }

  SpvMod m = {0};
  m.next_id = 1;
  m.program = program;
  m.device_function_ids =
      calloc(program->function_count ? program->function_count : 1,
             sizeof(*m.device_function_ids));
  m.function_builtin_masks =
      calloc(program->function_count ? program->function_count : 1,
             sizeof(*m.function_builtin_masks));
  if (!m.device_function_ids || !m.function_builtin_masks) {
    if (error) *error = strdup("out of memory emitting SPIR-V device functions");
    ir_gpu_call_graph_destroy(&graph);
    free(m.device_function_ids);
    free(m.function_builtin_masks);
    return 0;
  }
  /* Assign every reachable function id before emitting bodies. This keeps the
   * call representation independent of definition order, while the shared
   * postorder still emits callees first for deterministic modules. */
  for (size_t oi = 0; oi < graph.count; oi++) {
    m.device_function_ids[graph.order[oi]] = new_id(&m);
  }

  /* OpenCL.std extended instruction set (imported once, used by the f32 math
   * intrinsics; harmless if unused). */
  m.opencl_ext = new_id(&m);
  {
    Wb ei = {0};
    wb_push(&ei, m.opencl_ext);
    wb_str(&ei, "OpenCL.std");
    emit_ops(&m.extimports, Op_ExtInstImport, &ei);
    wb_free(&ei);
  }

  for (size_t oi = 0; oi < graph.count && !m.error; oi++) {
    size_t i = graph.order[oi];
    emit_device_function(&m, program->functions[i], i);
  }

  if (m.error) {
    if (error) *error = m.error;
    else free(m.error);
    goto cleanup;
  }

  /* capabilities depend on which widths/features the module actually used */
  emitv(&m.caps, Op_Capability, 1, (unsigned)Cap_Addresses);
  emitv(&m.caps, Op_Capability, 1, (unsigned)Cap_Kernel);
  emitv(&m.caps, Op_Capability, 1, (unsigned)Cap_Int64);
  if (m.use_subgroups)
    emitv(&m.caps, Op_Capability, 1, (unsigned)Cap_Groups);
  if (m.use_int8) emitv(&m.caps, Op_Capability, 1, (unsigned)Cap_Int8);
  if (m.use_int16) emitv(&m.caps, Op_Capability, 1, (unsigned)Cap_Int16);
  if (m.use_float16) emitv(&m.caps, Op_Capability, 1, (unsigned)Cap_Float16);
  if (m.use_float64) emitv(&m.caps, Op_Capability, 1, (unsigned)Cap_Float64);
  if (m.use_atomics64) emitv(&m.caps, Op_Capability, 1, (unsigned)Cap_Int64Atomics);
  if (m.use_subgroup_ballot)
    emitv(&m.caps, Op_Capability, 1, (unsigned)Cap_SubgroupBallotKHR);
  if (m.use_subgroup_vote)
    emitv(&m.caps, Op_Capability, 1, (unsigned)Cap_SubgroupVoteKHR);
  if (m.use_subgroup_ballot) {
    Wb extension = {0};
    wb_str(&extension, "SPV_KHR_shader_ballot");
    emit_ops(&m.caps, Op_Extension, &extension);
    wb_free(&extension);
  }
  if (m.use_subgroup_vote) {
    Wb extension = {0};
    wb_str(&extension, "SPV_KHR_subgroup_vote");
    emit_ops(&m.caps, Op_Extension, &extension);
    wb_free(&extension);
  }

  emitv(&m.memmodel, Op_MemoryModel, 2, (unsigned)AddrModel_Physical64,
        (unsigned)MemModel_OpenCL);

  /* ---- assemble the module in mandated layout order ---- */
  write_word_le(out, SPV_MAGIC);
  write_word_le(out, SPV_VERSION_1_0);
  write_word_le(out, 0u);         /* generator magic (0 = none) */
  write_word_le(out, m.next_id);  /* id bound */
  write_word_le(out, 0u);         /* schema */
  write_section(out, &m.caps);
  write_section(out, &m.extimports);
  write_section(out, &m.memmodel);
  write_section(out, &m.entrypoints);
  write_section(out, &m.execmodes);
  write_section(out, &m.decorations);
  write_section(out, &m.typesconsts);
  write_section(out, &m.functions);

cleanup:
  wb_free(&m.caps);
  wb_free(&m.extimports);
  wb_free(&m.memmodel);
  wb_free(&m.entrypoints);
  wb_free(&m.execmodes);
  wb_free(&m.decorations);
  wb_free(&m.typesconsts);
  wb_free(&m.functions);
  for (size_t i = 0; i < m.ncache; i++) free(m.cache[i].key);
  free(m.cache);
  free(m.device_function_ids);
  free(m.function_builtin_masks);
  ir_gpu_call_graph_destroy(&graph);
  return m.error ? 0 : 1;
}
