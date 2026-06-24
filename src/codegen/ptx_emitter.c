/* IR -> NVIDIA PTX text emitter. See ptx_emitter.h.
 *
 * Strategy: PTX is a typed virtual ISA with unlimited registers, so there is no
 * register allocation. Each IR value (SSA temp or mutable local/param) is bound
 * to one PTX register of a class derived from its type:
 *   PC_PRED -> %p   PC_B32 -> %r   PC_B64 -> %rd (also pointers)
 *   PC_F32  -> %f   PC_F64 -> %fd
 * Types come from parameter_types, DECLARE_LOCAL text, and per-instruction
 * inference of temps -- never from symbol-table lookups (which return NULL for
 * popped scopes at codegen time). Kernels only touch params + locals, so this
 * is self-contained. The function body is buffered so the .reg declarations
 * (which need the final per-class counts) can be emitted at the top. */
#include "ptx_emitter.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef enum { PC_NONE, PC_PRED, PC_B16, PC_B32, PC_B64, PC_F32, PC_F64 } PtxClass;

typedef struct {
  PtxClass cls;
  int idx;         /* register index within its class */
  int is_unsigned; /* integer signedness hint */
  int is_ptr;      /* pointer value (still PC_B64) */
  TypeKind elem;   /* pointed-to scalar kind, when is_ptr */
} PtxVal;

typedef struct {
  char *name;
  PtxVal val;
} PtxBinding;

/* growable text buffer */
typedef struct {
  char *data;
  size_t len, cap;
} Sb;

typedef struct {
  Sb body;
  int count[8]; /* register counts indexed by PtxClass */
  PtxBinding *binds;
  size_t nbinds, capbinds;
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

/* ---- register allocation (just bump a per-class counter) ---- */
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
static void reg_name(PtxClass c, int idx, char *out) {
  snprintf(out, 24, "%s%d", cls_prefix(c), idx);
}

/* ---- value bindings ---- */
static PtxBinding *find_binding(PtxFn *fn, const char *name) {
  for (size_t i = 0; i < fn->nbinds; i++) {
    if (strcmp(fn->binds[i].name, name) == 0) {
      return &fn->binds[i];
    }
  }
  return NULL;
}
static PtxVal *bind_value(PtxFn *fn, const char *name, PtxVal v) {
  PtxBinding *b = find_binding(fn, name);
  if (b) {
    b->val = v;
    return &b->val;
  }
  if (fn->nbinds == fn->capbinds) {
    fn->capbinds = fn->capbinds ? fn->capbinds * 2 : 16;
    fn->binds = realloc(fn->binds, fn->capbinds * sizeof(PtxBinding));
  }
  fn->binds[fn->nbinds].name = strdup(name);
  fn->binds[fn->nbinds].val = v;
  return &fn->binds[fn->nbinds++].val;
}

/* ---- type-name parsing (no symbol table) ---- */
static TypeKind base_kind_from_name(const char *s, int *is_unsigned) {
  *is_unsigned = (strstr(s, "uint") != NULL || strstr(s, "bool") != NULL);
  if (strstr(s, "float32") || strstr(s, "f32")) {
    return TYPE_FLOAT32;
  }
  if (strstr(s, "float64") || strstr(s, "double") || strstr(s, "float")) {
    return TYPE_FLOAT64;
  }
  if (strstr(s, "int64") || strstr(s, "uint64")) {
    return *is_unsigned ? TYPE_UINT64 : TYPE_INT64;
  }
  if (strstr(s, "int16") || strstr(s, "uint16")) {
    return *is_unsigned ? TYPE_UINT16 : TYPE_INT16;
  }
  if (strstr(s, "int8") || strstr(s, "uint8")) {
    return *is_unsigned ? TYPE_UINT8 : TYPE_INT8;
  }
  if (strstr(s, "int32") || strstr(s, "uint32")) {
    return *is_unsigned ? TYPE_UINT32 : TYPE_INT32;
  }
  if (strstr(s, "bool")) {
    return TYPE_BOOL;
  }
  return TYPE_INT32;
}
static PtxClass class_of_kind(TypeKind k, int *is_unsigned) {
  switch (k) {
  case TYPE_INT8:
  case TYPE_INT16:
  case TYPE_INT32:
    *is_unsigned = 0;
    return PC_B32;
  case TYPE_UINT8:
  case TYPE_UINT16:
  case TYPE_UINT32:
  case TYPE_BOOL:
    *is_unsigned = 1;
    return PC_B32;
  case TYPE_INT64:
    *is_unsigned = 0;
    return PC_B64;
  case TYPE_UINT64:
    *is_unsigned = 1;
    return PC_B64;
  case TYPE_FLOAT32:
    *is_unsigned = 0;
    return PC_F32;
  case TYPE_FLOAT64:
    *is_unsigned = 0;
    return PC_F64;
  case TYPE_POINTER:
  case TYPE_ARRAY:
  case TYPE_STRING:
  case TYPE_FUNCTION_POINTER:
    *is_unsigned = 1;
    return PC_B64;
  default:
    *is_unsigned = 0;
    return PC_B32;
  }
}
/* Build a PtxVal descriptor (class/ptr/elem) from a type-name string, without
 * allocating a register. */
static PtxVal descriptor_from_typename(const char *name) {
  PtxVal v = {0};
  if (!name) {
    name = "int64";
  }
  int ptr = (strchr(name, '*') != NULL) || strstr(name, "cstring") ||
            strstr(name, "string");
  int isu = 0;
  TypeKind base = base_kind_from_name(name, &isu);
  if (strstr(name, "cstring")) {
    base = TYPE_UINT8;
  }
  if (ptr) {
    v.cls = PC_B64;
    v.is_ptr = 1;
    v.is_unsigned = 1;
    /* pointer-to-pointer (two or more '*') -> element is itself a pointer */
    const char *firstStar = strchr(name, '*');
    v.elem = (firstStar && strchr(firstStar + 1, '*')) ? TYPE_POINTER : base;
  } else {
    int u = 0;
    v.cls = class_of_kind(base, &u);
    v.is_unsigned = u;
    v.elem = base;
  }
  return v;
}

/* element class from a pointer value's elem kind */
static PtxClass elem_class(TypeKind elem, int *is_unsigned) {
  return class_of_kind(elem, is_unsigned);
}

/* PTX ld/st type suffix for a load/store of element kind */
static const char *mem_type_suffix(TypeKind elem) {
  switch (elem) {
  case TYPE_INT8:
    return "s8";
  case TYPE_UINT8:
    return "u8";
  case TYPE_INT16:
    return "s16";
  case TYPE_UINT16:
    return "u16";
  case TYPE_INT32:
    return "s32";
  case TYPE_UINT32:
  case TYPE_BOOL:
    return "u32";
  case TYPE_INT64:
    return "s64";
  case TYPE_UINT64:
  case TYPE_POINTER:
  case TYPE_ARRAY:
  case TYPE_STRING:
  case TYPE_FUNCTION_POINTER:
    return "u64";
  case TYPE_FLOAT32:
    return "f32";
  case TYPE_FLOAT64:
    return "f64";
  default:
    return "u32";
  }
}

/* ---- conversions ---- */
/* Coerce a value in register (src) of class scls to class want; emits cvt as
 * needed; writes the resulting register name into out. */
static void coerce(PtxFn *fn, PtxClass scls, int s_unsigned, const char *srcreg,
                   PtxClass want, char *out) {
  if (scls == want) {
    snprintf(out, 24, "%s", srcreg);
    return;
  }
  int idx = new_reg(fn, want);
  reg_name(want, idx, out);
  /* integer width changes */
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

/* Resolve a source operand into a register of class `want`, materializing
 * immediates and coercing as needed. Writes the register name into out. */
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
  /* temp or symbol */
  PtxBinding *b = (op->name) ? find_binding(fn, op->name) : NULL;
  if (!b) {
    fn_error(fn, "PTX: use of undefined value '%s'",
             op->name ? op->name : "?");
    snprintf(out, 24, "%%r0");
    return;
  }
  char src[24];
  reg_name(b->val.cls, b->val.idx, src);
  coerce(fn, b->val.cls, b->val.is_unsigned, src, want, out);
}

/* class of an operand's current value (for result-type inference) */
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

/* ---- GPU index intrinsics ---- */
static const char *sreg_for_intrinsic(const char *name) {
  if (!name) {
    return NULL;
  }
  if (!strcmp(name, "gpu_tid_x")) return "%tid.x";
  if (!strcmp(name, "gpu_tid_y")) return "%tid.y";
  if (!strcmp(name, "gpu_tid_z")) return "%tid.z";
  if (!strcmp(name, "gpu_ntid_x")) return "%ntid.x";
  if (!strcmp(name, "gpu_ntid_y")) return "%ntid.y";
  if (!strcmp(name, "gpu_ntid_z")) return "%ntid.z";
  if (!strcmp(name, "gpu_ctaid_x")) return "%ctaid.x";
  if (!strcmp(name, "gpu_ctaid_y")) return "%ctaid.y";
  if (!strcmp(name, "gpu_ctaid_z")) return "%ctaid.z";
  if (!strcmp(name, "gpu_nctaid_x")) return "%nctaid.x";
  if (!strcmp(name, "gpu_nctaid_y")) return "%nctaid.y";
  if (!strcmp(name, "gpu_nctaid_z")) return "%nctaid.z";
  return NULL;
}

/* binary op classification */
static int is_compare_op(const char *t) {
  return !strcmp(t, "<") || !strcmp(t, ">") || !strcmp(t, "<=") ||
         !strcmp(t, ">=") || !strcmp(t, "==") || !strcmp(t, "!=");
}

static const char *setp_cmp(const char *t, int is_float, int is_unsigned) {
  /* returns the comparison mnemonic component */
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

static void emit_binary(PtxFn *fn, const IRInstruction *in);
static void emit_function(IRProgram *program, size_t fi, CodeGenerator *gen,
                          FILE *out, char **error);

int ptx_emit_program(IRProgram *program, CodeGenerator *generator, FILE *out,
                     char **error) {
  if (error) {
    *error = NULL;
  }
  if (!program || !out) {
    if (error) {
      *error = strdup("ptx_emit_program: null program/out");
    }
    return 0;
  }
  fprintf(out, "//\n// Generated by the Mettle PTX backend (--emit-ptx).\n//\n");
  fprintf(out, ".version 8.0\n.target sm_90\n.address_size 64\n\n");
  for (size_t i = 0; i < program->function_count; i++) {
    char *ferr = NULL;
    emit_function(program, i, generator, out, &ferr);
    if (ferr) {
      if (error) {
        *error = ferr;
      } else {
        free(ferr);
      }
      return 0;
    }
  }
  return 1;
}

/* Map a parameter/local type-name to the PTX .param storage type. */
static const char *param_storage_type(PtxClass cls) {
  switch (cls) {
  case PC_B64:
    return "u64";
  case PC_F32:
    return "f32";
  case PC_F64:
    return "f64";
  default:
    return "u32";
  }
}
static const char *ldparam_type(PtxClass cls) {
  switch (cls) {
  case PC_B64:
    return "u64";
  case PC_F32:
    return "f32";
  case PC_F64:
    return "f64";
  default:
    return "u32";
  }
}

static void emit_function(IRProgram *program, size_t fi, CodeGenerator *gen,
                          FILE *out, char **error) {
  (void)gen;
  IRFunction *func = program->functions[fi];
  PtxFn fn = {0};

  char ename[256];
  sanitize_into(func->name ? func->name : "kernel", ename, sizeof(ename));

  /* --- signature --- */
  Sb sig = {0};
  sb_printf(&sig, ".visible .entry %s(", ename);
  /* pre-bind parameters; load them at the top of the body */
  PtxVal *param_descs = calloc(func->parameter_count + 1, sizeof(PtxVal));
  for (size_t p = 0; p < func->parameter_count; p++) {
    const char *tn = func->parameter_types ? func->parameter_types[p] : NULL;
    PtxVal d = descriptor_from_typename(tn);
    param_descs[p] = d;
    if (p) {
      sb_puts(&sig, ",");
    }
    sb_printf(&sig, "\n    .param .%s %s_p%zu", param_storage_type(d.cls), ename,
              p);
  }
  sb_puts(&sig, "\n)\n");

  /* body: load params into registers and bind by name */
  for (size_t p = 0; p < func->parameter_count; p++) {
    PtxVal d = param_descs[p];
    d.idx = new_reg(&fn, d.cls);
    char rn[24];
    reg_name(d.cls, d.idx, rn);
    sb_printf(&fn.body, "\tld.param.%s %s, [%s_p%zu];\n", ldparam_type(d.cls),
              rn, ename, p);
    if (func->parameter_names && func->parameter_names[p]) {
      bind_value(&fn, func->parameter_names[p], d);
    }
  }

  /* --- walk instructions --- */
  for (size_t ii = 0; ii < func->instruction_count && !fn.error; ii++) {
    const IRInstruction *in = &func->instructions[ii];
    switch (in->op) {
    case IR_OP_NOP:
    case IR_OP_DECLARE_LOCAL: {
      if (in->op == IR_OP_DECLARE_LOCAL && in->dest.name) {
        /* pre-allocate a register for the local so refs resolve; aggregates or
         * address-taken locals are unsupported and will surface as errors. */
        PtxVal d = descriptor_from_typename(in->text);
        if (d.cls == PC_NONE) {
          fn_error(&fn, "PTX: local '%s' has unsupported type '%s'",
                   in->dest.name, in->text ? in->text : "?");
          break;
        }
        if (!find_binding(&fn, in->dest.name)) {
          d.idx = new_reg(&fn, d.cls);
          bind_value(&fn, in->dest.name, d);
        }
      }
      break;
    }
    case IR_OP_LABEL: {
      char lbl[256];
      sanitize_into(in->text ? in->text : "L", lbl, sizeof(lbl));
      sb_printf(&fn.body, "%s:\n", lbl);
      break;
    }
    case IR_OP_JUMP: {
      char lbl[256];
      sanitize_into(in->text ? in->text : "L", lbl, sizeof(lbl));
      sb_printf(&fn.body, "\tbra %s;\n", lbl);
      break;
    }
    case IR_OP_BRANCH_ZERO: {
      char lbl[256], r[24];
      sanitize_into(in->text ? in->text : "L", lbl, sizeof(lbl));
      PtxVal cv = operand_desc(&fn, &in->lhs);
      int p = new_reg(&fn, PC_PRED);
      char pn[24];
      reg_name(PC_PRED, p, pn);
      if (cv.cls == PC_F32 || cv.cls == PC_F64) {
        use_as(&fn, &in->lhs, cv.cls, r);
        /* Zero immediate as a hex bit-pattern: f32 needs 0f + 8 digits, f64
         * needs 0d + 16. (Hand-written literals are an easy off-by-one;
         * formatting from the bits is not.) */
        if (cv.cls == PC_F32) {
          sb_printf(&fn.body, "\tsetp.eq.f32 %s, %s, 0f%08X;\n", pn, r, 0u);
        } else {
          sb_printf(&fn.body, "\tsetp.eq.f64 %s, %s, 0d%016llX;\n", pn, r, 0ull);
        }
      } else {
        PtxClass c = (cv.cls == PC_B64) ? PC_B64 : PC_B32;
        use_as(&fn, &in->lhs, c, r);
        sb_printf(&fn.body, "\tsetp.eq.%s %s, %s, 0;\n",
                  c == PC_B64 ? "s64" : "s32", pn, r);
      }
      sb_printf(&fn.body, "\t@%s bra %s;\n", pn, lbl);
      break;
    }
    case IR_OP_BRANCH_EQ: {
      char lbl[256], a[24], bb[24];
      sanitize_into(in->text ? in->text : "L", lbl, sizeof(lbl));
      PtxVal la = operand_desc(&fn, &in->lhs);
      PtxVal lb = operand_desc(&fn, &in->rhs);
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
      use_as(&fn, &in->lhs, c, a);
      use_as(&fn, &in->rhs, c, bb);
      int p = new_reg(&fn, PC_PRED);
      char pn[24];
      reg_name(PC_PRED, p, pn);
      sb_printf(&fn.body, "\tsetp.eq.%s %s, %s, %s;\n",
                type_suffix_for_class(c, 0), pn, a, bb);
      sb_printf(&fn.body, "\t@%s bra %s;\n", pn, lbl);
      break;
    }
    case IR_OP_ASSIGN: {
      if (!in->dest.name) {
        fn_error(&fn, "PTX: assign with no dest");
        break;
      }
      /* destination class: reuse if symbol already bound, else infer from src */
      PtxBinding *db = find_binding(&fn, in->dest.name);
      PtxClass dc;
      if (db) {
        dc = db->val.cls;
      } else {
        PtxVal sv = operand_desc(&fn, &in->lhs);
        dc = (sv.cls == PC_NONE || sv.cls == PC_PRED) ? PC_B32 : sv.cls;
      }
      char src[24];
      use_as(&fn, &in->lhs, dc, src);
      PtxVal dv;
      if (db) {
        dv = db->val;
      } else {
        dv = operand_desc(&fn, &in->lhs);
        dv.cls = dc;
        dv.idx = new_reg(&fn, dc);
      }
      char dn[24];
      reg_name(dv.cls, dv.idx, dn);
      if (strcmp(dn, src) != 0) {
        const char *mt = (dc == PC_F32)   ? "f32"
                         : (dc == PC_F64) ? "f64"
                         : (dc == PC_B64) ? "u64"
                                          : "u32";
        sb_printf(&fn.body, "\tmov.%s %s, %s;\n", mt, dn, src);
      }
      bind_value(&fn, in->dest.name, dv);
      break;
    }
    case IR_OP_LOAD: {
      /* dest <- *lhs [rhs size] */
      PtxVal addr = operand_desc(&fn, &in->lhs);
      TypeKind elem = addr.is_ptr ? addr.elem : TYPE_VOID;
      if (elem == TYPE_VOID) {
        /* fall back to size + is_float */
        long long sz = (in->rhs.kind == IR_OPERAND_INT) ? in->rhs.int_value : 4;
        if (in->is_float) {
          elem = (sz == 4) ? TYPE_FLOAT32 : TYPE_FLOAT64;
        } else {
          elem = (sz == 8) ? TYPE_INT64
                 : (sz == 2) ? TYPE_INT16
                 : (sz == 1) ? TYPE_UINT8
                             : TYPE_INT32;
        }
      }
      char addrreg[24];
      use_as(&fn, &in->lhs, PC_B64, addrreg);
      int u = 0;
      PtxClass dc = elem_class(elem, &u);
      PtxVal dv = {0};
      dv.cls = dc;
      dv.is_unsigned = u;
      dv.idx = new_reg(&fn, dc);
      char dn[24];
      reg_name(dc, dv.idx, dn);
      sb_printf(&fn.body, "\tld.global.%s %s, [%s];\n", mem_type_suffix(elem),
                dn, addrreg);
      if (in->dest.name) {
        bind_value(&fn, in->dest.name, dv);
      }
      break;
    }
    case IR_OP_STORE: {
      /* *dest <- lhs [rhs size] */
      PtxVal addr = operand_desc(&fn, &in->dest);
      TypeKind elem = addr.is_ptr ? addr.elem : TYPE_VOID;
      if (elem == TYPE_VOID) {
        long long sz = (in->rhs.kind == IR_OPERAND_INT) ? in->rhs.int_value : 4;
        if (in->is_float) {
          elem = (sz == 4) ? TYPE_FLOAT32 : TYPE_FLOAT64;
        } else {
          elem = (sz == 8) ? TYPE_INT64
                 : (sz == 2) ? TYPE_INT16
                 : (sz == 1) ? TYPE_UINT8
                             : TYPE_INT32;
        }
      }
      int u = 0;
      PtxClass vc = elem_class(elem, &u);
      char addrreg[24], valreg[24];
      use_as(&fn, &in->dest, PC_B64, addrreg);
      use_as(&fn, &in->lhs, vc, valreg);
      sb_printf(&fn.body, "\tst.global.%s [%s], %s;\n", mem_type_suffix(elem),
                addrreg, valreg);
      break;
    }
    case IR_OP_BINARY:
      emit_binary(&fn, in);
      break;
    case IR_OP_UNARY: {
      const char *t = in->text ? in->text : "";
      PtxVal sv = operand_desc(&fn, &in->lhs);
      PtxClass c = in->is_float ? (in->float_bits == 32 ? PC_F32 : PC_F64)
                                : (sv.cls == PC_B64 ? PC_B64 : PC_B32);
      char s[24];
      use_as(&fn, &in->lhs, c, s);
      PtxVal dv = {0};
      dv.cls = c;
      dv.idx = new_reg(&fn, c);
      char dn[24];
      reg_name(c, dv.idx, dn);
      if (!strcmp(t, "-")) {
        sb_printf(&fn.body, "\tneg.%s %s, %s;\n", type_suffix_for_class(c, 0),
                  dn, s);
      } else if (!strcmp(t, "~")) {
        sb_printf(&fn.body, "\tnot.%s %s, %s;\n", c == PC_B64 ? "b64" : "b32",
                  dn, s);
      } else if (!strcmp(t, "!")) {
        int p = new_reg(&fn, PC_PRED);
        char pn[24];
        reg_name(PC_PRED, p, pn);
        sb_printf(&fn.body, "\tsetp.eq.%s %s, %s, 0;\n",
                  c == PC_B64 ? "s64" : "s32", pn, s);
        sb_printf(&fn.body, "\tselp.u32 %s, 1, 0, %s;\n", dn, pn);
      } else {
        fn_error(&fn, "PTX: unsupported unary op '%s'", t);
      }
      if (in->dest.name) {
        bind_value(&fn, in->dest.name, dv);
      }
      break;
    }
    case IR_OP_CAST: {
      /* dest = (text) lhs */
      PtxVal target = descriptor_from_typename(in->text);
      char s[24];
      use_as(&fn, &in->lhs, target.cls, s);
      target.idx = new_reg(&fn, target.cls);
      char dn[24];
      reg_name(target.cls, target.idx, dn);
      if (strcmp(dn, s) != 0) {
        const char *mt = (target.cls == PC_F32)   ? "f32"
                         : (target.cls == PC_F64) ? "f64"
                         : (target.cls == PC_B64) ? "u64"
                                                  : "u32";
        sb_printf(&fn.body, "\tmov.%s %s, %s;\n", mt, dn, s);
      }
      if (in->dest.name) {
        bind_value(&fn, in->dest.name, target);
      }
      break;
    }
    case IR_OP_CALL: {
      const char *callee = in->text;
      const char *sreg = sreg_for_intrinsic(callee);
      if (sreg) {
        PtxVal dv = {0};
        dv.cls = PC_B32;
        dv.is_unsigned = 1;
        dv.idx = new_reg(&fn, PC_B32);
        char dn[24];
        reg_name(PC_B32, dv.idx, dn);
        sb_printf(&fn.body, "\tmov.u32 %s, %s;\n", dn, sreg);
        if (in->dest.name) {
          bind_value(&fn, in->dest.name, dv);
        }
      } else if (callee && !strcmp(callee, "gpu_barrier")) {
        sb_puts(&fn.body, "\tbar.sync 0;\n");
      } else if (callee && in->argument_count >= 1 && !strcmp(callee, "h2f")) {
        /* h2f(bits): reinterpret a uint16 fp16 bit-pattern as float32. The arg
         * arrives as a 32-bit int (zero-extended u16 load); truncate to .b16 and
         * cvt.f32.f16. Lets prefill keep fp16-resident weights and convert on
         * the fly with one PTX instruction. */
        char a[24];
        use_as(&fn, &in->arguments[0], PC_B32, a);
        int hidx = new_reg(&fn, PC_B16);
        char hn[24];
        reg_name(PC_B16, hidx, hn);
        sb_printf(&fn.body, "\tcvt.u16.u32 %s, %s;\n", hn, a);
        PtxVal dv = {.cls = PC_F32, .idx = new_reg(&fn, PC_F32)};
        char dn[24];
        reg_name(PC_F32, dv.idx, dn);
        sb_printf(&fn.body, "\tcvt.f32.f16 %s, %s;\n", dn, hn);
        if (in->dest.name)
          bind_value(&fn, in->dest.name, dv);
      } else if (callee && in->argument_count >= 1 && !strcmp(callee, "f2h")) {
        /* f2h(x): float32 -> uint16 fp16 bit-pattern (cvt.rn.f16.f32), returned
         * zero-extended in a 32-bit int so a uint16 store writes the low 16. */
        char a[24];
        use_as(&fn, &in->arguments[0], PC_F32, a);
        int hidx = new_reg(&fn, PC_B16);
        char hn[24];
        reg_name(PC_B16, hidx, hn);
        sb_printf(&fn.body, "\tcvt.rn.f16.f32 %s, %s;\n", hn, a);
        PtxVal dv = {.cls = PC_B32, .is_unsigned = 1, .idx = new_reg(&fn, PC_B32)};
        char dn[24];
        reg_name(PC_B32, dv.idx, dn);
        sb_printf(&fn.body, "\tcvt.u32.u16 %s, %s;\n", dn, hn);
        if (in->dest.name)
          bind_value(&fn, in->dest.name, dv);
      } else if (callee && in->argument_count >= 1 &&
                 (!strcmp(callee, "sqrtf") || !strcmp(callee, "expf") ||
                  !strcmp(callee, "sinf") || !strcmp(callee, "cosf") ||
                  !strcmp(callee, "rsqrtf") || !strcmp(callee, "fabsf") ||
                  !strcmp(callee, "logf"))) {
        /* single-arg f32 math -> PTX approximations (inference-grade, mirrors
         * the fast CPU approximations the engine already uses). */
        char a[24];
        use_as(&fn, &in->arguments[0], PC_F32, a);
        PtxVal dv = {.cls = PC_F32, .idx = new_reg(&fn, PC_F32)};
        char dn[24];
        reg_name(PC_F32, dv.idx, dn);
        if (!strcmp(callee, "sqrtf")) {
          sb_printf(&fn.body, "\tsqrt.rn.f32 %s, %s;\n", dn, a);
        } else if (!strcmp(callee, "rsqrtf")) {
          sb_printf(&fn.body, "\trsqrt.approx.f32 %s, %s;\n", dn, a);
        } else if (!strcmp(callee, "fabsf")) {
          sb_printf(&fn.body, "\tabs.f32 %s, %s;\n", dn, a);
        } else if (!strcmp(callee, "sinf")) {
          sb_printf(&fn.body, "\tsin.approx.f32 %s, %s;\n", dn, a);
        } else if (!strcmp(callee, "cosf")) {
          sb_printf(&fn.body, "\tcos.approx.f32 %s, %s;\n", dn, a);
        } else if (!strcmp(callee, "logf")) {
          /* ln(x) = lg2(x) / log2(e) = lg2(x) * 0.6931471805599453 */
          int t = new_reg(&fn, PC_F32);
          char tn[24];
          reg_name(PC_F32, t, tn);
          sb_printf(&fn.body, "\tlg2.approx.f32 %s, %s;\n", tn, a);
          sb_printf(&fn.body, "\tmul.f32 %s, %s, 0f3F317218;\n", dn, tn);
        } else { /* expf: exp(x) = 2^(x * log2(e)), log2(e)=1.4426950408 */
          int t = new_reg(&fn, PC_F32);
          char tn[24];
          reg_name(PC_F32, t, tn);
          sb_printf(&fn.body, "\tmul.f32 %s, %s, 0f3FB8AA3B;\n", tn, a);
          sb_printf(&fn.body, "\tex2.approx.f32 %s, %s;\n", dn, tn);
        }
        if (in->dest.name)
          bind_value(&fn, in->dest.name, dv);
      } else if (callee && in->argument_count >= 3 &&
                 (!strcmp(callee, "atomic_min_u32") ||
                  !strcmp(callee, "atomic_min_u64") ||
                  !strcmp(callee, "atomic_add_u32"))) {
        /* atomic_{min,add}_uXX(buf, idx, val) -> old: unsigned atomic op into
         * buf[idx]. Address is computed here (idx*elem + base) so kernels need
         * no address-of/pointer arithmetic. Returns the previous value. */
        int is64 = !strcmp(callee, "atomic_min_u64");
        const char *opn = !strcmp(callee, "atomic_add_u32") ? "add" : "min";
        PtxClass vc = is64 ? PC_B64 : PC_B32;
        int elem = is64 ? 8 : 4;
        char bufr[24], idxr[24], valr[24];
        use_as(&fn, &in->arguments[0], PC_B64, bufr);
        use_as(&fn, &in->arguments[1], PC_B32, idxr);
        use_as(&fn, &in->arguments[2], vc, valr);
        int addr = new_reg(&fn, PC_B64);
        char an[24];
        reg_name(PC_B64, addr, an);
        sb_printf(&fn.body, "\tmad.wide.s32 %s, %s, %d, %s;\n", an, idxr, elem,
                  bufr);
        PtxVal dv = {.cls = vc, .is_unsigned = 1, .idx = new_reg(&fn, vc)};
        char dn[24];
        reg_name(vc, dv.idx, dn);
        sb_printf(&fn.body, "\tatom.global.%s.%s %s, [%s], %s;\n", opn,
                  is64 ? "u64" : "u32", dn, an, valr);
        if (in->dest.name)
          bind_value(&fn, in->dest.name, dv);
      } else {
        fn_error(&fn, "PTX: unsupported call '%s'", callee ? callee : "?");
      }
      break;
    }
    case IR_OP_RETURN:
      sb_puts(&fn.body, "\tret;\n");
      break;
    case IR_OP_ADDRESS_OF:
      fn_error(&fn, "PTX: address-of (&local) not supported in kernels yet");
      break;
    default:
      fn_error(&fn, "PTX: unsupported IR opcode %d in kernel '%s'", in->op,
               func->name ? func->name : "?");
      break;
    }
  }

  if (fn.error) {
    if (error) {
      *error = fn.error;
    } else {
      free(fn.error);
    }
    /* cleanup */
    free(sig.data);
    free(fn.body.data);
    free(param_descs);
    for (size_t i = 0; i < fn.nbinds; i++) {
      free(fn.binds[i].name);
    }
    free(fn.binds);
    return;
  }

  /* --- assemble: signature { reg-decls body } --- */
  fputs(sig.data, out);
  fputs("{\n", out);
  static const PtxClass classes[6] = {PC_PRED, PC_B16, PC_B32,
                                      PC_B64,  PC_F32, PC_F64};
  for (int c = 0; c < 6; c++) {
    PtxClass cc = classes[c];
    if (fn.count[cc] > 0) {
      fprintf(out, "\t.reg %s %s<%d>;\n", cls_regtype(cc), cls_prefix(cc),
              fn.count[cc]);
    }
  }
  fputs(fn.body.data ? fn.body.data : "", out);
  /* ensure a trailing ret */
  fputs("\tret;\n}\n\n", out);

  free(sig.data);
  free(fn.body.data);
  free(param_descs);
  for (size_t i = 0; i < fn.nbinds; i++) {
    free(fn.binds[i].name);
  }
  free(fn.binds);
}

/* ---- BINARY ---- */
static void emit_binary(PtxFn *fn, const IRInstruction *in) {
  const char *t = in->text ? in->text : "+";
  PtxVal la = operand_desc(fn, &in->lhs);
  PtxVal ra = operand_desc(fn, &in->rhs);

  if (is_compare_op(t)) {
    /* operand compare class */
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
      /* C "usual arithmetic conversions": if either operand is unsigned the
       * comparison is unsigned. (Integer literals carry is_unsigned=0, so `&&`
       * here would wrongly make `unsigned_var < 10` a signed compare.) */
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
    PtxVal dv = {.cls = PC_B32, .is_unsigned = 1, .idx = new_reg(fn, PC_B32)};
    char dn[24];
    reg_name(PC_B32, dv.idx, dn);
    sb_printf(&fn->body, "\tselp.u32 %s, 1, 0, %s;\n", dn, pn);
    if (in->dest.name) {
      bind_value(fn, in->dest.name, dv);
    }
    return;
  }

  /* logical && / || : treat as bitwise on 0/1 ints */
  int is_logical = (!strcmp(t, "&&") || !strcmp(t, "||"));

  /* result class */
  PtxVal dv = {0};
  if (in->is_float) {
    dv.cls = (in->float_bits == 32) ? PC_F32 : PC_F64;
  } else if (la.is_ptr || ra.is_ptr) {
    /* pointer arithmetic: result is a pointer, element from whichever side */
    dv.cls = PC_B64;
    dv.is_ptr = 1;
    dv.is_unsigned = 1;
    dv.elem = la.is_ptr ? la.elem : ra.elem;
  } else {
    dv.cls = (la.cls == PC_B64 || ra.cls == PC_B64) ? PC_B64 : PC_B32;
    /* Unsigned if either operand is unsigned (C usual arithmetic conversions).
     * `&&` is wrong: integer literals are is_unsigned=0, so `unsigned_var / 7`
     * or `unsigned_var >> 3` would emit signed div/shr and miscompute for
     * high-bit-set values. */
    dv.is_unsigned = la.is_unsigned || ra.is_unsigned;
  }

  char a[24], b[24];
  use_as(fn, &in->lhs, dv.cls, a);
  use_as(fn, &in->rhs, dv.cls, b);
  dv.idx = new_reg(fn, dv.cls);
  char dn[24];
  reg_name(dv.cls, dv.idx, dn);

  const char *ts = type_suffix_for_class(dv.cls, dv.is_unsigned);
  const char *bts = (dv.cls == PC_B64) ? "b64" : "b32";

  if (!strcmp(t, "+")) {
    sb_printf(&fn->body, "\tadd.%s %s, %s, %s;\n", ts, dn, a, b);
  } else if (!strcmp(t, "-")) {
    sb_printf(&fn->body, "\tsub.%s %s, %s, %s;\n", ts, dn, a, b);
  } else if (!strcmp(t, "*")) {
    if (dv.cls == PC_F32 || dv.cls == PC_F64) {
      sb_printf(&fn->body, "\tmul.%s %s, %s, %s;\n", ts, dn, a, b);
    } else {
      sb_printf(&fn->body, "\tmul.lo.%s %s, %s, %s;\n", ts, dn, a, b);
    }
  } else if (!strcmp(t, "/")) {
    if (dv.cls == PC_F32) {
      sb_printf(&fn->body, "\tdiv.rn.f32 %s, %s, %s;\n", dn, a, b);
    } else if (dv.cls == PC_F64) {
      sb_printf(&fn->body, "\tdiv.rn.f64 %s, %s, %s;\n", dn, a, b);
    } else {
      sb_printf(&fn->body, "\tdiv.%s %s, %s, %s;\n", ts, dn, a, b);
    }
  } else if (!strcmp(t, "%")) {
    sb_printf(&fn->body, "\trem.%s %s, %s, %s;\n", ts, dn, a, b);
  } else if (!strcmp(t, "&") || (is_logical && !strcmp(t, "&&"))) {
    sb_printf(&fn->body, "\tand.%s %s, %s, %s;\n", bts, dn, a, b);
  } else if (!strcmp(t, "|") || (is_logical && !strcmp(t, "||"))) {
    sb_printf(&fn->body, "\tor.%s %s, %s, %s;\n", bts, dn, a, b);
  } else if (!strcmp(t, "^")) {
    sb_printf(&fn->body, "\txor.%s %s, %s, %s;\n", bts, dn, a, b);
  } else if (!strcmp(t, "<<")) {
    /* shift amount is a .u32 in PTX */
    char sh[24];
    use_as(fn, &in->rhs, PC_B32, sh);
    sb_printf(&fn->body, "\tshl.%s %s, %s, %s;\n", bts, dn, a, sh);
  } else if (!strcmp(t, ">>")) {
    /* Arithmetic (signed) vs logical (unsigned) shift is decided by the value
     * being shifted -- the left operand -- not the shift count. */
    char sh[24];
    use_as(fn, &in->rhs, PC_B32, sh);
    sb_printf(&fn->body, "\tshr.%s %s, %s, %s;\n",
              type_suffix_for_class(dv.cls, la.is_unsigned), dn, a, sh);
  } else {
    fn_error(fn, "PTX: unsupported binary op '%s'", t);
  }
  if (in->dest.name) {
    bind_value(fn, in->dest.name, dv);
  }
}
