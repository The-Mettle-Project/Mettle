/* Reference interpreter for optimized IR: the semantic arbiter behind
 * --verify. See ir_interp.h for the model. Every opcode implementation here
 * encodes the DOCUMENTED semantics from ir.h; if a pass emits IR whose real
 * meaning differs from what it documents, the before/after comparison in
 * ir_verify.c diverges and the pass is caught. */
#include "ir_interp.h"
#include "../common.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define II_ADDR_BASE 0x0000200000000000ULL
#define II_ADDR_STRIDE 0x0000000001000000ULL /* 16 MiB per buffer slot */
#define II_MAX_BUFFERS 512
#define II_MAX_DEPTH 48
#define II_TRACE_CAP 512
#define II_MAX_BUFFER_SIZE (4LL * 1024 * 1024)

typedef struct {
  unsigned long long base;
  unsigned char *data;
  long long size;
  int freed;
  size_t alloc_line; /* NEW/malloc source line; 0 for harness inputs */
} IIBuffer;

typedef struct {
  const char *key; /* not owned; lives as long as the IR */
  IRInterpValue value;
  /* Slot-backed local: reads/writes go through memory at value.i. */
  int slotted;
  int slot_size;
  int slot_is_float;
  int slot_is_unsigned;
} IIVar;

typedef struct {
  IIVar *vars;
  size_t count;
  size_t capacity; /* power of two; 0 = empty */
} IIEnv;

struct IRInterpMachine {
  IRProgram *program;
  const char *override_name;
  IRFunction *override_fn;

  /* Grown on demand (capped at II_MAX_BUFFERS / II_TRACE_CAP). Embedding the
   * full-capacity arrays made the machine struct ~570 KB, and translation
   * validation creates tens of thousands of machines per compile - the calloc
   * zeroing alone dominated --verify wall time. Entries are fully initialized
   * on write, so the grown storage is plain malloc. */
  IIBuffer *buffers;
  size_t buffer_count;
  size_t buffer_capacity;

  IIEnv globals;

  IRInterpExternCall *trace;
  size_t trace_count;
  size_t trace_capacity;

  long long fuel;
  int depth;
  IRInterpStatus status;
  char detail[128];

  /* Source location of the CALL currently dispatching to an extern; used to
   * attribute assert failures and heap allocations to source lines. */
  SourceLocation current_call_loc;

  /* assert()/assert_eq() failure details (mettle test). */
  int assert_failed;
  size_t assert_line;
  size_t assert_column;
  IRInterpValue assert_left;
  IRInterpValue assert_right;
  int assert_is_eq;

  /* Value tracing (mettle trace). */
  IRInterpValueHook value_hook;
  void *value_hook_ctx;
  const IRFunction *value_hook_fn;

  /* Execution counting (zero-run PGO). */
  int count_enabled;
  struct {
    const IRFunction *fn;
    long long *counts;
    size_t n;
  } *count_tables;
  size_t count_table_count;
  size_t count_table_capacity;
};

/* ---------------- environment ---------------- */

static size_t ii_hash(const char *s) {
  size_t h = 1469598103934665603ull;
  while (*s) {
    h ^= (unsigned char)*s++;
    h *= 1099511628211ull;
  }
  return h;
}

static void ii_env_free(IIEnv *env) {
  free(env->vars);
  env->vars = NULL;
  env->count = 0;
  env->capacity = 0;
}

static IIVar *ii_env_slot(IIEnv *env, const char *key) {
  size_t mask = env->capacity - 1;
  size_t i = ii_hash(key) & mask;
  while (env->vars[i].key) {
    if (strcmp(env->vars[i].key, key) == 0) {
      return &env->vars[i];
    }
    i = (i + 1) & mask;
  }
  return &env->vars[i];
}

static int ii_env_grow(IIEnv *env) {
  size_t new_capacity = env->capacity ? env->capacity * 2 : 64;
  IIVar *old = env->vars;
  size_t old_capacity = env->capacity;
  IIVar *grown = (IIVar *)calloc(new_capacity, sizeof(IIVar));
  if (!grown) {
    return 0;
  }
  env->vars = grown;
  env->capacity = new_capacity;
  env->count = 0;
  for (size_t i = 0; i < old_capacity; i++) {
    if (old[i].key) {
      IIVar *slot = ii_env_slot(env, old[i].key);
      *slot = old[i];
      env->count++;
    }
  }
  free(old);
  return 1;
}

/* Find existing entry or NULL. */
static IIVar *ii_env_find(IIEnv *env, const char *key) {
  if (!env->capacity) {
    return NULL;
  }
  IIVar *slot = ii_env_slot(env, key);
  return slot->key ? slot : NULL;
}

/* Find or insert (zero value). Returns NULL only on OOM. */
static IIVar *ii_env_upsert(IIEnv *env, const char *key) {
  if (env->capacity == 0 || env->count * 10 >= env->capacity * 7) {
    if (!ii_env_grow(env)) {
      return NULL;
    }
  }
  IIVar *slot = ii_env_slot(env, key);
  if (!slot->key) {
    slot->key = key;
    memset(&slot->value, 0, sizeof(slot->value));
    slot->slotted = 0;
    env->count++;
  }
  return slot;
}

/* ---------------- machine ---------------- */

IRInterpMachine *ir_interp_create(IRProgram *program) {
  IRInterpMachine *machine = (IRInterpMachine *)calloc(1, sizeof(*machine));
  if (!machine) {
    return NULL;
  }
  machine->program = program;
  machine->status = IR_INTERP_OK;
  return machine;
}

void ir_interp_destroy(IRInterpMachine *machine) {
  if (!machine) {
    return;
  }
  for (size_t i = 0; i < machine->buffer_count; i++) {
    free(machine->buffers[i].data);
  }
  free(machine->buffers);
  free(machine->trace);
  for (size_t i = 0; i < machine->count_table_count; i++) {
    free(machine->count_tables[i].counts);
  }
  free(machine->count_tables);
  ii_env_free(&machine->globals);
  free(machine);
}

void ir_interp_enable_counting(IRInterpMachine *machine) {
  if (machine) {
    machine->count_enabled = 1;
  }
}

static long long *ii_counts_for(IRInterpMachine *machine,
                                const IRFunction *fn) {
  if (!machine->count_enabled || !fn || fn->instruction_count == 0) {
    return NULL;
  }
  for (size_t i = 0; i < machine->count_table_count; i++) {
    if (machine->count_tables[i].fn == fn) {
      return machine->count_tables[i].counts;
    }
  }
  if (machine->count_table_count >= machine->count_table_capacity) {
    size_t new_capacity =
        machine->count_table_capacity ? machine->count_table_capacity * 2 : 32;
    void *grown = realloc(machine->count_tables,
                          new_capacity * sizeof(*machine->count_tables));
    if (!grown) {
      return NULL;
    }
    machine->count_tables = grown;
    machine->count_table_capacity = new_capacity;
  }
  long long *counts =
      (long long *)calloc(fn->instruction_count, sizeof(long long));
  if (!counts) {
    return NULL;
  }
  machine->count_tables[machine->count_table_count].fn = fn;
  machine->count_tables[machine->count_table_count].counts = counts;
  machine->count_tables[machine->count_table_count].n = fn->instruction_count;
  machine->count_table_count++;
  return counts;
}

const long long *ir_interp_get_counts(const IRInterpMachine *machine,
                                      const IRFunction *function,
                                      size_t *count_out) {
  if (!machine || !function) {
    return NULL;
  }
  for (size_t i = 0; i < machine->count_table_count; i++) {
    if (machine->count_tables[i].fn == function) {
      if (count_out) {
        *count_out = machine->count_tables[i].n;
      }
      return machine->count_tables[i].counts;
    }
  }
  return NULL;
}

void ir_interp_set_override(IRInterpMachine *machine, const char *name,
                            IRFunction *fn) {
  if (!machine) {
    return;
  }
  machine->override_name = name;
  machine->override_fn = fn;
}

static void ii_fail(IRInterpMachine *machine, IRInterpStatus status,
                    const char *detail) {
  if (machine->status == IR_INTERP_OK) {
    machine->status = status;
    snprintf(machine->detail, sizeof(machine->detail), "%s",
             detail ? detail : "");
  }
}

unsigned long long ir_interp_add_buffer(IRInterpMachine *machine,
                                        const void *init, long long size) {
  if (!machine || size < 0 || size > II_MAX_BUFFER_SIZE ||
      machine->buffer_count >= II_MAX_BUFFERS) {
    return 0;
  }
  if (machine->buffer_count == machine->buffer_capacity) {
    size_t grown = machine->buffer_capacity ? machine->buffer_capacity * 2 : 16;
    IIBuffer *table =
        (IIBuffer *)realloc(machine->buffers, grown * sizeof(IIBuffer));
    if (!table) {
      return 0;
    }
    machine->buffers = table;
    machine->buffer_capacity = grown;
  }
  IIBuffer *buf = &machine->buffers[machine->buffer_count];
  buf->size = size;
  buf->freed = 0;
  buf->alloc_line = 0;
  buf->base = II_ADDR_BASE + (unsigned long long)machine->buffer_count *
                                 II_ADDR_STRIDE;
  buf->data = (unsigned char *)malloc(size > 0 ? (size_t)size : 1);
  if (!buf->data) {
    return 0;
  }
  if (init) {
    memcpy(buf->data, init, (size_t)size);
  } else {
    memset(buf->data, 0, (size_t)size);
  }
  machine->buffer_count++;
  return buf->base;
}

static IIBuffer *ii_addr_to_buffer(IRInterpMachine *machine,
                                   unsigned long long addr, long long size,
                                   long long *offset_out) {
  if (addr < II_ADDR_BASE) {
    return NULL;
  }
  unsigned long long index = (addr - II_ADDR_BASE) / II_ADDR_STRIDE;
  if (index >= machine->buffer_count) {
    return NULL;
  }
  IIBuffer *buf = &machine->buffers[index];
  long long offset = (long long)(addr - buf->base);
  if (buf->freed || offset < 0 || size < 0 || offset + size > buf->size) {
    return NULL;
  }
  *offset_out = offset;
  return buf;
}

static int ii_mem_read(IRInterpMachine *machine, unsigned long long addr,
                       int size, unsigned long long *out) {
  long long offset = 0;
  IIBuffer *buf = ii_addr_to_buffer(machine, addr, size, &offset);
  if (!buf) {
    ii_fail(machine, IR_INTERP_TRAP, "load out of bounds / after free");
    return 0;
  }
  unsigned long long value = 0;
  memcpy(&value, buf->data + offset, (size_t)size);
  *out = value;
  return 1;
}

static int ii_mem_write(IRInterpMachine *machine, unsigned long long addr,
                        int size, unsigned long long value) {
  long long offset = 0;
  IIBuffer *buf = ii_addr_to_buffer(machine, addr, size, &offset);
  if (!buf) {
    ii_fail(machine, IR_INTERP_TRAP, "store out of bounds / after free");
    return 0;
  }
  memcpy(buf->data + offset, &value, (size_t)size);
  return 1;
}

/* Typed element helpers for the SIMD kernels. */
static int ii_read_i32(IRInterpMachine *m, unsigned long long a, int *v) {
  unsigned long long raw;
  if (!ii_mem_read(m, a, 4, &raw)) return 0;
  *v = (int)(unsigned int)raw;
  return 1;
}
static int ii_write_i32(IRInterpMachine *m, unsigned long long a, int v) {
  return ii_mem_write(m, a, 4, (unsigned int)v);
}
static int ii_read_f64(IRInterpMachine *m, unsigned long long a, double *v) {
  unsigned long long raw;
  if (!ii_mem_read(m, a, 8, &raw)) return 0;
  memcpy(v, &raw, 8);
  return 1;
}
static int ii_write_f64(IRInterpMachine *m, unsigned long long a, double v) {
  unsigned long long raw;
  memcpy(&raw, &v, 8);
  return ii_mem_write(m, a, 8, raw);
}
static int ii_read_f32(IRInterpMachine *m, unsigned long long a, float *v) {
  unsigned long long raw;
  if (!ii_mem_read(m, a, 4, &raw)) return 0;
  unsigned int bits = (unsigned int)raw;
  memcpy(v, &bits, 4);
  return 1;
}
static int ii_write_f32(IRInterpMachine *m, unsigned long long a, float v) {
  unsigned int bits;
  memcpy(&bits, &v, 4);
  return ii_mem_write(m, a, 4, bits);
}

/* ---------------- frames ---------------- */

typedef struct {
  const char *label;
  size_t index;
} IILabel;

typedef struct {
  IIEnv env;
  IILabel *labels;
  size_t label_count;
  IRFunction *fn;
} IIFrame;

static const IILabel *ii_find_label(const IIFrame *frame, const char *name) {
  for (size_t i = 0; i < frame->label_count; i++) {
    if (strcmp(frame->labels[i].label, name) == 0) {
      return &frame->labels[i];
    }
  }
  return NULL;
}

/* Parse a DECLARE_LOCAL type string. Returns 1 on success with element size,
 * count (1 for scalars), float-ness, unsignedness; 0 for uninterpretable
 * types (structs, strings, closures). */
static int ii_parse_local_type(const char *text, int *elem_size, long long *count,
                               int *is_float, int *is_unsigned) {
  static const struct {
    const char *name;
    int size;
    int is_float;
    int is_unsigned;
  } SCALARS[] = {
      {"int8", 1, 0, 0},    {"int16", 2, 0, 0},   {"int32", 4, 0, 0},
      {"int64", 8, 0, 0},   {"uint8", 1, 0, 1},   {"uint16", 2, 0, 1},
      {"uint32", 4, 0, 1},  {"uint64", 8, 0, 1},  {"bool", 1, 0, 1},
      {"float32", 4, 1, 0}, {"float64", 8, 1, 0},
  };
  if (!text) {
    return 0;
  }
  size_t len = strlen(text);
  *count = 1;
  /* Pointer-typed local: an 8-byte scalar value. */
  if (len > 0 && text[len - 1] == '*') {
    *elem_size = 8;
    *is_float = 0;
    *is_unsigned = 1;
    return 1;
  }
  char base[32];
  const char *bracket = strchr(text, '[');
  if (bracket) {
    size_t base_len = (size_t)(bracket - text);
    if (base_len >= sizeof(base)) {
      return 0;
    }
    memcpy(base, text, base_len);
    base[base_len] = '\0';
    long long n = atoll(bracket + 1);
    if (n <= 0 || n > (1 << 20)) {
      return 0;
    }
    *count = n;
  } else {
    if (len >= sizeof(base)) {
      return 0;
    }
    memcpy(base, text, len + 1);
  }
  for (size_t i = 0; i < sizeof(SCALARS) / sizeof(SCALARS[0]); i++) {
    if (strcmp(base, SCALARS[i].name) == 0) {
      *elem_size = SCALARS[i].size;
      *is_float = SCALARS[i].is_float;
      *is_unsigned = SCALARS[i].is_unsigned;
      return 1;
    }
  }
  return 0;
}

/* ---------------- value plumbing ---------------- */

static long long ii_as_int(const IRInterpValue *value) {
  return value->is_float ? (long long)value->f : value->i;
}

static double ii_as_float(const IRInterpValue *value) {
  return value->is_float ? value->f : (double)value->i;
}

static IRInterpValue ii_int_value(long long v) {
  IRInterpValue value;
  value.i = v;
  value.f = 0;
  value.is_float = 0;
  return value;
}

static IRInterpValue ii_float_value(double v) {
  IRInterpValue value;
  value.i = 0;
  value.f = v;
  value.is_float = 1;
  return value;
}

/* Uninitialized locals/temps read this instead of 0. Native code gives them
 * stack or register garbage, so a zero-defaulting interpreter would blind the
 * differential to a deleted initializing store (`@neg <- 0` in print_int was
 * exactly that). Deterministic, identical in both machines - only a transform
 * that changes WHETHER a read sees its initialization can diverge on it. */
#define II_POISON_BYTE 0xA5
static IRInterpValue ii_poison_value(void) {
  IRInterpValue value;
  value.i = (long long)0xA5A5A5A5A5A5A5A5ULL;
  value.f = 0;
  value.is_float = 0;
  return value;
}

/* Equality for assert_eq: exact for ints; floats compare as doubles (a test
 * author asserting float equality means bit-for-bit intent). */
static int ii_value_matches(const IRInterpValue *a, const IRInterpValue *b) {
  if (a->is_float || b->is_float) {
    double x = a->is_float ? a->f : (double)a->i;
    double y = b->is_float ? b->f : (double)b->i;
    return x == y;
  }
  return a->i == b->i;
}

static int ii_exec_function(IRInterpMachine *machine, IRFunction *fn,
                            const IRInterpValue *args, size_t arg_count,
                            IRInterpValue *result);

/* Read a variable, honoring slot-backed locals. */
static int ii_var_read(IRInterpMachine *machine, IIVar *var,
                       IRInterpValue *out) {
  if (!var->slotted) {
    *out = var->value;
    return 1;
  }
  unsigned long long raw = 0;
  if (!ii_mem_read(machine, (unsigned long long)var->value.i, var->slot_size,
                   &raw)) {
    return 0;
  }
  if (var->slot_is_float) {
    if (var->slot_size == 4) {
      float f;
      unsigned int bits = (unsigned int)raw;
      memcpy(&f, &bits, 4);
      *out = ii_float_value((double)f);
    } else {
      double d;
      memcpy(&d, &raw, 8);
      *out = ii_float_value(d);
    }
    return 1;
  }
  long long v = (long long)raw;
  if (!var->slot_is_unsigned && var->slot_size < 8) {
    int shift = 64 - var->slot_size * 8;
    v = (v << shift) >> shift;
  }
  *out = ii_int_value(v);
  return 1;
}

static int ii_var_write(IRInterpMachine *machine, IIVar *var,
                        const IRInterpValue *value) {
  if (!var->slotted) {
    var->value = *value;
    return 1;
  }
  unsigned long long raw = 0;
  if (var->slot_is_float) {
    if (var->slot_size == 4) {
      float f = (float)ii_as_float(value);
      unsigned int bits;
      memcpy(&bits, &f, 4);
      raw = bits;
    } else {
      double d = ii_as_float(value);
      memcpy(&raw, &d, 8);
    }
  } else {
    raw = (unsigned long long)ii_as_int(value);
  }
  return ii_mem_write(machine, (unsigned long long)var->value.i,
                      var->slot_size, raw);
}

/* Resolve a TEMP/SYMBOL operand name to its variable: frame first, then
 * globals (creating there on first touch, matching "undeclared symbol is a
 * global" lowering). */
static IIVar *ii_resolve(IRInterpMachine *machine, IIFrame *frame,
                         const IROperand *operand) {
  IIVar *var = ii_env_find(&frame->env, operand->name);
  if (var) {
    return var;
  }
  if (operand->kind == IR_OPERAND_TEMP) {
    /* Temps are function-local by construction. A brand-new temp slot means a
     * read before any def (possible only after a transform deleted the def):
     * it reads POISON, not 0, because native code would see a stale register.
     * A deterministic-but-nonzero value keeps the differential faithful and
     * both machines identical. Globals stay zero (.bss is zeroed for real). */
    IIVar *fresh = ii_env_upsert(&frame->env, operand->name);
    if (fresh) {
      fresh->value = ii_poison_value();
    }
    return fresh;
  }
  return ii_env_upsert(&machine->globals, operand->name);
}

static int ii_fetch(IRInterpMachine *machine, IIFrame *frame,
                    const IROperand *operand, IRInterpValue *out) {
  switch (operand->kind) {
  case IR_OPERAND_NONE:
    *out = ii_int_value(0);
    return 1;
  case IR_OPERAND_INT:
    *out = ii_int_value(operand->int_value);
    return 1;
  case IR_OPERAND_FLOAT:
    *out = ii_float_value(operand->float_value);
    return 1;
  case IR_OPERAND_TEMP:
  case IR_OPERAND_SYMBOL: {
    if (!operand->name) {
      *out = ii_int_value(0);
      return 1;
    }
    IIVar *var = ii_resolve(machine, frame, operand);
    if (!var) {
      ii_fail(machine, IR_INTERP_TRAP, "out of memory");
      return 0;
    }
    return ii_var_read(machine, var, out);
  }
  default:
    ii_fail(machine, IR_INTERP_UNSUPPORTED, "string/label operand as value");
    return 0;
  }
}

static int ii_store_dest(IRInterpMachine *machine, IIFrame *frame,
                         const IROperand *dest, const IRInterpValue *value) {
  if (dest->kind == IR_OPERAND_NONE) {
    return 1;
  }
  if ((dest->kind != IR_OPERAND_TEMP && dest->kind != IR_OPERAND_SYMBOL) ||
      !dest->name) {
    ii_fail(machine, IR_INTERP_UNSUPPORTED, "unwritable destination operand");
    return 0;
  }
  IIVar *var = ii_resolve(machine, frame, dest);
  if (!var) {
    ii_fail(machine, IR_INTERP_TRAP, "out of memory");
    return 0;
  }
  return ii_var_write(machine, var, value);
}

/* Fetch as address (integer). */
static int ii_fetch_addr(IRInterpMachine *machine, IIFrame *frame,
                         const IROperand *operand, unsigned long long *out) {
  IRInterpValue value;
  if (!ii_fetch(machine, frame, operand, &value)) {
    return 0;
  }
  *out = (unsigned long long)ii_as_int(&value);
  return 1;
}

static int ii_fetch_int(IRInterpMachine *machine, IIFrame *frame,
                        const IROperand *operand, long long *out) {
  IRInterpValue value;
  if (!ii_fetch(machine, frame, operand, &value)) {
    return 0;
  }
  *out = ii_as_int(&value);
  return 1;
}

/* ---------------- scalar ops ---------------- */

static int ii_binary(IRInterpMachine *machine, const IRInstruction *insn,
                     const IRInterpValue *a, const IRInterpValue *b,
                     IRInterpValue *out) {
  const char *op = insn->text ? insn->text : "?";

  if (insn->is_float) {
    double x = ii_as_float(a);
    double y = ii_as_float(b);
    int narrow = insn->float_bits == 32;
    if (strcmp(op, "+") == 0 || strcmp(op, "-") == 0 || strcmp(op, "*") == 0 ||
        strcmp(op, "/") == 0) {
      double r;
      if (narrow) {
        float fx = (float)x, fy = (float)y;
        float fr = op[0] == '+' ? fx + fy
                   : op[0] == '-' ? fx - fy
                   : op[0] == '*' ? fx * fy
                                  : fx / fy;
        r = (double)fr;
      } else {
        r = op[0] == '+' ? x + y : op[0] == '-' ? x - y : op[0] == '*' ? x * y : x / y;
      }
      *out = ii_float_value(r);
      return 1;
    }
    if (strcmp(op, "==") == 0) { *out = ii_int_value(x == y); return 1; }
    if (strcmp(op, "!=") == 0) { *out = ii_int_value(x != y); return 1; }
    if (strcmp(op, "<") == 0)  { *out = ii_int_value(x < y);  return 1; }
    if (strcmp(op, "<=") == 0) { *out = ii_int_value(x <= y); return 1; }
    if (strcmp(op, ">") == 0)  { *out = ii_int_value(x > y);  return 1; }
    if (strcmp(op, ">=") == 0) { *out = ii_int_value(x >= y); return 1; }
    ii_fail(machine, IR_INTERP_UNSUPPORTED, "float binary op");
    return 0;
  }

  long long sx = ii_as_int(a);
  long long sy = ii_as_int(b);
  unsigned long long ux = (unsigned long long)sx;
  unsigned long long uy = (unsigned long long)sy;
  int is_unsigned = insn->is_unsigned;

  if (strcmp(op, "+") == 0) { *out = ii_int_value((long long)(ux + uy)); return 1; }
  if (strcmp(op, "-") == 0) { *out = ii_int_value((long long)(ux - uy)); return 1; }
  if (strcmp(op, "*") == 0) { *out = ii_int_value((long long)(ux * uy)); return 1; }
  if (strcmp(op, "&") == 0) { *out = ii_int_value((long long)(ux & uy)); return 1; }
  if (strcmp(op, "|") == 0) { *out = ii_int_value((long long)(ux | uy)); return 1; }
  if (strcmp(op, "^") == 0) { *out = ii_int_value((long long)(ux ^ uy)); return 1; }
  if (strcmp(op, "<<") == 0) {
    *out = ii_int_value((long long)(ux << (uy & 63)));
    return 1;
  }
  if (strcmp(op, ">>") == 0) {
    if (is_unsigned) {
      *out = ii_int_value((long long)(ux >> (uy & 63)));
    } else {
      *out = ii_int_value(sx >> (uy & 63));
    }
    return 1;
  }
  if (strcmp(op, "/") == 0 || strcmp(op, "%") == 0) {
    if (sy == 0 || (!is_unsigned && sx == LLONG_MIN && sy == -1)) {
      ii_fail(machine, IR_INTERP_TRAP, "integer divide trap");
      return 0;
    }
    long long q;
    if (is_unsigned) {
      q = op[0] == '/' ? (long long)(ux / uy) : (long long)(ux % uy);
    } else {
      q = op[0] == '/' ? sx / sy : sx % sy;
    }
    *out = ii_int_value(q);
    return 1;
  }
  if (strcmp(op, "==") == 0) { *out = ii_int_value(sx == sy); return 1; }
  if (strcmp(op, "!=") == 0) { *out = ii_int_value(sx != sy); return 1; }
  if (strcmp(op, "<") == 0) {
    *out = ii_int_value(is_unsigned ? ux < uy : sx < sy);
    return 1;
  }
  if (strcmp(op, "<=") == 0) {
    *out = ii_int_value(is_unsigned ? ux <= uy : sx <= sy);
    return 1;
  }
  if (strcmp(op, ">") == 0) {
    *out = ii_int_value(is_unsigned ? ux > uy : sx > sy);
    return 1;
  }
  if (strcmp(op, ">=") == 0) {
    *out = ii_int_value(is_unsigned ? ux >= uy : sx >= sy);
    return 1;
  }
  if (strcmp(op, "&&") == 0) { *out = ii_int_value(sx != 0 && sy != 0); return 1; }
  if (strcmp(op, "||") == 0) { *out = ii_int_value(sx != 0 || sy != 0); return 1; }
  ii_fail(machine, IR_INTERP_UNSUPPORTED, "binary op");
  return 0;
}

static int ii_cast(IRInterpMachine *machine, const IRInstruction *insn,
                   const IRInterpValue *in, IRInterpValue *out) {
  const char *type = insn->text ? insn->text : "";
  size_t len = strlen(type);
  if (len > 0 && type[len - 1] == '*') {
    /* Pointer cast: value-preserving. */
    *out = ii_int_value(ii_as_int(in));
    return 1;
  }
  if (strcmp(type, "float64") == 0) {
    *out = ii_float_value(ii_as_float(in));
    return 1;
  }
  if (strcmp(type, "float32") == 0) {
    *out = ii_float_value((double)(float)ii_as_float(in));
    return 1;
  }
  int size = 0, target_unsigned = 0;
  if (strcmp(type, "int8") == 0) { size = 1; }
  else if (strcmp(type, "int16") == 0) { size = 2; }
  else if (strcmp(type, "int32") == 0) { size = 4; }
  else if (strcmp(type, "int64") == 0) { size = 8; }
  else if (strcmp(type, "uint8") == 0) { size = 1; target_unsigned = 1; }
  else if (strcmp(type, "uint16") == 0) { size = 2; target_unsigned = 1; }
  else if (strcmp(type, "uint32") == 0) { size = 4; target_unsigned = 1; }
  else if (strcmp(type, "uint64") == 0) { size = 8; target_unsigned = 1; }
  else if (strcmp(type, "bool") == 0) {
    *out = ii_int_value(in->is_float ? in->f != 0.0 : in->i != 0);
    return 1;
  } else {
    ii_fail(machine, IR_INTERP_UNSUPPORTED, "cast target type");
    return 0;
  }

  long long v;
  if (in->is_float) {
    /* Float -> int: truncate toward zero; x86 cvtt sentinel on overflow/NaN. */
    double d = in->f;
    if (size == 8 || size == 4) {
      double lo = size == 8 ? -9223372036854775808.0 : -2147483648.0;
      double hi = size == 8 ? 9223372036854775808.0 : 2147483648.0;
      if (!(d >= lo && d < hi)) {
        v = size == 8 ? LLONG_MIN : (long long)INT_MIN;
      } else {
        v = (long long)d;
      }
    } else {
      v = (long long)d;
    }
  } else {
    v = in->i;
  }
  if (size < 8) {
    int shift = 64 - size * 8;
    if (target_unsigned) {
      v = (long long)(((unsigned long long)v << shift) >> shift);
    } else {
      v = (v << shift) >> shift;
    }
  }
  *out = ii_int_value(v);
  return 1;
}

/* ---------------- extern model ---------------- */

static void ii_trace_extern(IRInterpMachine *machine, const char *name,
                            const IRInterpValue *args, size_t arg_count) {
  if (machine->trace_count >= II_TRACE_CAP) {
    ii_fail(machine, IR_INTERP_UNSUPPORTED, "extern trace overflow");
    return;
  }
  if (machine->trace_count == machine->trace_capacity) {
    size_t grown = machine->trace_capacity ? machine->trace_capacity * 2 : 8;
    IRInterpExternCall *table = (IRInterpExternCall *)realloc(
        machine->trace, grown * sizeof(IRInterpExternCall));
    if (!table) {
      ii_fail(machine, IR_INTERP_TRAP, "out of memory");
      return;
    }
    machine->trace = table;
    machine->trace_capacity = grown;
  }
  IRInterpExternCall *call = &machine->trace[machine->trace_count++];
  snprintf(call->name, sizeof(call->name), "%s", name);
  call->arg_count = arg_count > 8 ? 8 : arg_count;
  for (size_t i = 0; i < call->arg_count; i++) {
    call->args[i] = args[i];
    call->arg_mem_len[i] = 0;
    if (args[i].is_float) {
      continue;
    }
    /* Pointer argument: capture the bytes it addresses right now, up to the
     * cap or the end of its buffer. The extern observes memory at the moment
     * of the call, so the trace must too. */
    long long offset = 0;
    IIBuffer *buf = ii_addr_to_buffer(machine, (unsigned long long)args[i].i,
                                      0, &offset);
    if (!buf) {
      continue;
    }
    long long avail = buf->size - offset;
    if (avail <= 0) {
      continue;
    }
    long long take =
        avail < IR_INTERP_EXTERN_MEM_CAP ? avail : IR_INTERP_EXTERN_MEM_CAP;
    memcpy(call->arg_mem[i], buf->data + offset, (size_t)take);
    call->arg_mem_len[i] = (unsigned short)take;
  }
}

static IRFunction *ii_find_function(IRInterpMachine *machine,
                                    const char *name) {
  if (machine->override_name && strcmp(machine->override_name, name) == 0) {
    return machine->override_fn;
  }
  if (!machine->program) {
    return NULL;
  }
  for (size_t i = 0; i < machine->program->function_count; i++) {
    IRFunction *fn = machine->program->functions[i];
    if (fn && fn->name && strcmp(fn->name, name) == 0) {
      return fn;
    }
  }
  return NULL;
}

static long long ii_buffer_size_at(IRInterpMachine *machine,
                                   unsigned long long addr) {
  long long offset = 0;
  IIBuffer *buf = ii_addr_to_buffer(machine, addr, 0, &offset);
  if (!buf || offset != 0) {
    return -1;
  }
  return buf->size;
}

/* Modeled externs return 1 and set *result; unknown externs are traced and
 * return 0 (meaning: use the pure default). Returns -1 on trap. */
static int ii_extern_call(IRInterpMachine *machine, const char *name,
                          const IRInterpValue *args, size_t arg_count,
                          IRInterpValue *result) {
  *result = ii_int_value(0);

  if ((strcmp(name, "malloc") == 0 && arg_count == 1) ||
      (strcmp(name, "calloc") == 0 && arg_count == 2)) {
    long long size = ii_as_int(&args[0]);
    if (arg_count == 2) {
      size *= ii_as_int(&args[1]);
    }
    if (size < 0 || size > II_MAX_BUFFER_SIZE) {
      return -1;
    }
    unsigned long long addr = ir_interp_add_buffer(machine, NULL, size);
    if (!addr) {
      return -1;
    }
    if (strcmp(name, "malloc") == 0) {
      /* Deterministic "uninitialized" pattern. */
      long long offset = 0;
      IIBuffer *buf = ii_addr_to_buffer(machine, addr, 0, &offset);
      if (buf) {
        memset(buf->data, 0xA5, (size_t)buf->size);
      }
    }
    *result = ii_int_value((long long)addr);
    return 1;
  }
  if (strcmp(name, "free") == 0 && arg_count == 1) {
    unsigned long long addr = (unsigned long long)ii_as_int(&args[0]);
    if (addr == 0) {
      return 1;
    }
    long long offset = 0;
    IIBuffer *buf = ii_addr_to_buffer(machine, addr, 0, &offset);
    if (!buf || offset != 0) {
      return -1;
    }
    buf->freed = 1;
    return 1;
  }
  if (strcmp(name, "realloc") == 0 && arg_count == 2) {
    unsigned long long old_addr = (unsigned long long)ii_as_int(&args[0]);
    long long new_size = ii_as_int(&args[1]);
    if (new_size < 0 || new_size > II_MAX_BUFFER_SIZE) {
      return -1;
    }
    long long old_size = old_addr ? ii_buffer_size_at(machine, old_addr) : 0;
    if (old_addr && old_size < 0) {
      return -1;
    }
    unsigned long long addr = ir_interp_add_buffer(machine, NULL, new_size);
    if (!addr) {
      return -1;
    }
    if (old_addr) {
      long long copy = old_size < new_size ? old_size : new_size;
      long long src_off = 0, dst_off = 0;
      IIBuffer *src = ii_addr_to_buffer(machine, old_addr, copy, &src_off);
      IIBuffer *dst = ii_addr_to_buffer(machine, addr, copy, &dst_off);
      if (src && dst) {
        memcpy(dst->data + dst_off, src->data + src_off, (size_t)copy);
      }
      if (src) {
        src->freed = 1;
      }
    }
    *result = ii_int_value((long long)addr);
    return 1;
  }
  if ((strcmp(name, "memcpy") == 0 || strcmp(name, "memmove") == 0) &&
      arg_count == 3) {
    unsigned long long dst = (unsigned long long)ii_as_int(&args[0]);
    unsigned long long src = (unsigned long long)ii_as_int(&args[1]);
    long long n = ii_as_int(&args[2]);
    long long dst_off = 0, src_off = 0;
    IIBuffer *dbuf = ii_addr_to_buffer(machine, dst, n, &dst_off);
    IIBuffer *sbuf = ii_addr_to_buffer(machine, src, n, &src_off);
    if (!dbuf || !sbuf || n < 0) {
      return -1;
    }
    memmove(dbuf->data + dst_off, sbuf->data + src_off, (size_t)n);
    *result = ii_int_value((long long)dst);
    return 1;
  }
  if (strcmp(name, "memset") == 0 && arg_count == 3) {
    unsigned long long dst = (unsigned long long)ii_as_int(&args[0]);
    long long fill = ii_as_int(&args[1]);
    long long n = ii_as_int(&args[2]);
    long long dst_off = 0;
    IIBuffer *dbuf = ii_addr_to_buffer(machine, dst, n, &dst_off);
    if (!dbuf || n < 0) {
      return -1;
    }
    memset(dbuf->data + dst_off, (int)(fill & 0xFF), (size_t)n);
    *result = ii_int_value((long long)dst);
    return 1;
  }
  if (strcmp(name, "memcmp") == 0 && arg_count == 3) {
    unsigned long long a = (unsigned long long)ii_as_int(&args[0]);
    unsigned long long b = (unsigned long long)ii_as_int(&args[1]);
    long long n = ii_as_int(&args[2]);
    long long a_off = 0, b_off = 0;
    IIBuffer *abuf = ii_addr_to_buffer(machine, a, n, &a_off);
    IIBuffer *bbuf = ii_addr_to_buffer(machine, b, n, &b_off);
    if (!abuf || !bbuf || n < 0) {
      return -1;
    }
    *result = ii_int_value(memcmp(abuf->data + a_off, bbuf->data + b_off,
                                  (size_t)n));
    return 1;
  }

  if (strcmp(name, "strlen") == 0 && arg_count == 1) {
    unsigned long long addr = (unsigned long long)ii_as_int(&args[0]);
    long long offset = 0;
    IIBuffer *buf = ii_addr_to_buffer(machine, addr, 1, &offset);
    if (!buf) {
      return -1;
    }
    long long n = 0;
    while (offset + n < buf->size && buf->data[offset + n] != 0) {
      n++;
    }
    if (offset + n >= buf->size) {
      return -1; /* unterminated within the buffer: refuse to guess */
    }
    *result = ii_int_value(n);
    return 1;
  }

  /* assert()/assert_eq() builtins: interpreted natively by `mettle test`. */
  if (strcmp(name, "assert_eq") == 0 && arg_count == 2) {
    if (!ii_value_matches(&args[0], &args[1])) {
      machine->assert_failed = 1;
      machine->assert_line = machine->current_call_loc.line;
      machine->assert_column = machine->current_call_loc.column;
      machine->assert_left = args[0];
      machine->assert_right = args[1];
      machine->assert_is_eq = 1;
      ii_fail(machine, IR_INTERP_ASSERT_FAIL, "assert_eq");
      return -1;
    }
    return 1;
  }
  if (strcmp(name, "assert") == 0 && arg_count == 1) {
    long long cond = args[0].is_float ? (args[0].f != 0.0) : (args[0].i != 0);
    if (!cond) {
      machine->assert_failed = 1;
      machine->assert_line = machine->current_call_loc.line;
      machine->assert_column = machine->current_call_loc.column;
      machine->assert_left = args[0];
      machine->assert_right = ii_int_value(0);
      machine->assert_is_eq = 0;
      ii_fail(machine, IR_INTERP_ASSERT_FAIL, "assert");
      return -1;
    }
    return 1;
  }

  /* Runtime guard traps (null-check, bounds) abort the program. */
  if (strncmp(name, "mettle_crash_trap", 17) == 0) {
    ii_fail(machine, IR_INTERP_GUARD_TRAP, name);
    return -1;
  }

  /* Unknown extern: pure model (returns 0), but the call is traced so a pass
   * that deletes or reorders it still diverges. */
  ii_trace_extern(machine, name, args, arg_count);
  return machine->status == IR_INTERP_OK ? 0 : -1;
}

/* ---------------- SIMD kernel ops (documented scalar semantics) ---------- */

static int ii_exec_simd(IRInterpMachine *machine, IIFrame *frame,
                        const IRInstruction *insn) {
  switch (insn->op) {
  case IR_OP_PREFETCH:
    /* Advisory cache hint: no architectural effect, nothing to interpret. */
    return 1;
  case IR_OP_MEMCPY_INLINE: {
    unsigned long long dst, src;
    long long n;
    if (!ii_fetch_addr(machine, frame, &insn->dest, &dst) ||
        !ii_fetch_addr(machine, frame, &insn->lhs, &src) ||
        !ii_fetch_int(machine, frame, &insn->rhs, &n)) {
      return 0;
    }
    long long dst_off = 0, src_off = 0;
    IIBuffer *dbuf = ii_addr_to_buffer(machine, dst, n, &dst_off);
    IIBuffer *sbuf = ii_addr_to_buffer(machine, src, n, &src_off);
    if (!dbuf || !sbuf || n < 0) {
      ii_fail(machine, IR_INTERP_TRAP, "memcpy_inline out of bounds");
      return 0;
    }
    memmove(dbuf->data + dst_off, sbuf->data + src_off, (size_t)n);
    machine->fuel -= n;
    return 1;
  }

  case IR_OP_COUNT_WORD_STARTS: {
    unsigned long long base;
    long long n;
    IRInterpValue acc;
    if (!ii_fetch_addr(machine, frame, &insn->lhs, &base) ||
        !ii_fetch_int(machine, frame, &insn->rhs, &n) ||
        !ii_fetch(machine, frame, &insn->dest, &acc)) {
      return 0;
    }
    long long count = 0;
    int in_word = 0;
    for (long long i = 0; i < n; i++) {
      unsigned long long byte;
      if (!ii_mem_read(machine, base + (unsigned long long)i, 1, &byte)) {
        return 0;
      }
      int ws = byte == 0x20 || byte == 0x09 || byte == 0x0A || byte == 0x0D;
      if (ws) {
        in_word = 0;
      } else {
        if (!in_word) {
          count++;
        }
        in_word = 1;
      }
    }
    machine->fuel -= n;
    IRInterpValue out = ii_int_value(ii_as_int(&acc) + count);
    return ii_store_dest(machine, frame, &insn->dest, &out);
  }

  case IR_OP_SIMD_SUM_I32:
  case IR_OP_SIMD_SUM_U8: {
    unsigned long long base;
    long long n;
    IRInterpValue acc;
    if (!ii_fetch_addr(machine, frame, &insn->lhs, &base) ||
        !ii_fetch_int(machine, frame, &insn->rhs, &n) ||
        !ii_fetch(machine, frame, &insn->dest, &acc)) {
      return 0;
    }
    long long sum = 0;
    for (long long i = 0; i < n; i++) {
      if (insn->op == IR_OP_SIMD_SUM_I32) {
        int v;
        if (!ii_read_i32(machine, base + (unsigned long long)i * 4, &v)) {
          return 0;
        }
        sum += v;
      } else {
        unsigned long long v;
        if (!ii_mem_read(machine, base + (unsigned long long)i, 1, &v)) {
          return 0;
        }
        sum += (long long)v;
      }
    }
    machine->fuel -= n;
    IRInterpValue out = ii_int_value(ii_as_int(&acc) + sum);
    return ii_store_dest(machine, frame, &insn->dest, &out);
  }

  case IR_OP_SIMD_BYTE_MAP: {
    unsigned long long base;
    long long n;
    if (!ii_fetch_addr(machine, frame, &insn->lhs, &base) ||
        !ii_fetch_int(machine, frame, &insn->rhs, &n)) {
      return 0;
    }
    for (long long i = 0; i < n; i++) {
      unsigned long long raw;
      if (!ii_mem_read(machine, base + (unsigned long long)i, 1, &raw)) {
        return 0;
      }
      unsigned int b = (unsigned int)raw & 0xFF;
      for (size_t s = 0; s + 1 < insn->argument_count; s += 2) {
        long long code = insn->arguments[s].int_value;
        unsigned int k = (unsigned int)insn->arguments[s + 1].int_value & 0xFF;
        switch (code) {
        case IR_BYTE_MAP_ADD: b = (b + k) & 0xFF; break;
        case IR_BYTE_MAP_SUB: b = (b - k) & 0xFF; break;
        case IR_BYTE_MAP_MUL: b = (b * k) & 0xFF; break;
        case IR_BYTE_MAP_XOR: b = (b ^ k) & 0xFF; break;
        case IR_BYTE_MAP_AND: b = (b & k) & 0xFF; break;
        case IR_BYTE_MAP_OR:  b = (b | k) & 0xFF; break;
        default:
          ii_fail(machine, IR_INTERP_UNSUPPORTED, "byte_map op");
          return 0;
        }
      }
      if (!ii_mem_write(machine, base + (unsigned long long)i, 1, b)) {
        return 0;
      }
    }
    machine->fuel -= n;
    return 1;
  }

  case IR_OP_SIMD_FILL: {
    if (insn->argument_count < 3) {
      ii_fail(machine, IR_INTERP_UNSUPPORTED, "simd_fill arity");
      return 0;
    }
    long long elem_size = insn->arguments[0].int_value;
    long long mode = insn->arguments[1].int_value;
    if (elem_size != 1 && elem_size != 2 && elem_size != 4 && elem_size != 8) {
      ii_fail(machine, IR_INTERP_UNSUPPORTED, "simd_fill element size");
      return 0;
    }
    unsigned long long fill_bits = 0;
    if (insn->arguments[2].kind == IR_OPERAND_INT) {
      fill_bits = (unsigned long long)insn->arguments[2].int_value;
    } else {
      IRInterpValue v;
      if (!ii_fetch(machine, frame, &insn->arguments[2], &v)) {
        return 0;
      }
      if (v.is_float) {
        if (elem_size == 4) {
          float f = (float)v.f;
          unsigned int bits;
          memcpy(&bits, &f, 4);
          fill_bits = bits;
        } else {
          memcpy(&fill_bits, &v.f, 8);
        }
      } else {
        fill_bits = (unsigned long long)v.i;
      }
    }
    if (mode == 0) {
      unsigned long long base;
      long long bound, start = 0, offset = 0;
      if (!ii_fetch_addr(machine, frame, &insn->lhs, &base) ||
          !ii_fetch_int(machine, frame, &insn->rhs, &bound)) {
        return 0;
      }
      if (insn->argument_count > 3 &&
          !ii_fetch_int(machine, frame, &insn->arguments[3], &start)) {
        return 0;
      }
      if (insn->argument_count > 4 &&
          !ii_fetch_int(machine, frame, &insn->arguments[4], &offset)) {
        return 0;
      }
      for (long long i = start; i < bound; i++) {
        int idx32 = (int)(offset + i); /* 32-bit index math, like the loop */
        unsigned long long addr =
            base + (unsigned long long)((long long)idx32 * elem_size);
        if (!ii_mem_write(machine, addr, (int)elem_size, fill_bits)) {
          return 0;
        }
      }
      machine->fuel -= bound > start ? bound - start : 0;
      return 1;
    }
    if (mode == 1) {
      unsigned long long begin, end;
      if (!ii_fetch_addr(machine, frame, &insn->lhs, &begin) ||
          !ii_fetch_addr(machine, frame, &insn->rhs, &end)) {
        return 0;
      }
      long long steps = 0;
      for (unsigned long long p = begin; p < end; p += (unsigned long long)elem_size) {
        if (!ii_mem_write(machine, p, (int)elem_size, fill_bits)) {
          return 0;
        }
        steps++;
      }
      machine->fuel -= steps;
      return 1;
    }
    if (mode == 2) {
      unsigned long long base;
      long long bound, start = 0;
      if (!ii_fetch_addr(machine, frame, &insn->lhs, &base) ||
          !ii_fetch_int(machine, frame, &insn->rhs, &bound)) {
        return 0;
      }
      if (insn->argument_count > 3 &&
          !ii_fetch_int(machine, frame, &insn->arguments[3], &start)) {
        return 0;
      }
      long long steps = 0;
      for (long long off = start; off < bound; off += elem_size) {
        if (!ii_mem_write(machine, base + (unsigned long long)off,
                          (int)elem_size, fill_bits)) {
          return 0;
        }
        steps++;
      }
      machine->fuel -= steps;
      return 1;
    }
    ii_fail(machine, IR_INTERP_UNSUPPORTED, "simd_fill mode");
    return 0;
  }

  case IR_OP_SIMD_INSERTION_SORT_I32: {
    unsigned long long base;
    long long n;
    if (!ii_fetch_addr(machine, frame, &insn->dest, &base) ||
        !ii_fetch_int(machine, frame, &insn->rhs, &n)) {
      return 0;
    }
    for (long long i = 1; i < n; i++) {
      int key;
      if (!ii_read_i32(machine, base + (unsigned long long)i * 4, &key)) {
        return 0;
      }
      long long j = i - 1;
      while (j >= 0) {
        int v;
        if (!ii_read_i32(machine, base + (unsigned long long)j * 4, &v)) {
          return 0;
        }
        if (v <= key) {
          break;
        }
        if (!ii_write_i32(machine, base + (unsigned long long)(j + 1) * 4, v)) {
          return 0;
        }
        j--;
        machine->fuel--;
      }
      if (!ii_write_i32(machine, base + (unsigned long long)(j + 1) * 4, key)) {
        return 0;
      }
    }
    machine->fuel -= n;
    return 1;
  }

  case IR_OP_SIMD_DOT_I32: {
    unsigned long long a, b;
    long long n;
    IRInterpValue acc;
    if (insn->argument_count < 1 ||
        !ii_fetch_addr(machine, frame, &insn->lhs, &a) ||
        !ii_fetch_addr(machine, frame, &insn->rhs, &b) ||
        !ii_fetch_int(machine, frame, &insn->arguments[0], &n) ||
        !ii_fetch(machine, frame, &insn->dest, &acc)) {
      return 0;
    }
    long long sum = 0;
    for (long long i = 0; i < n; i++) {
      int x, y;
      if (!ii_read_i32(machine, a + (unsigned long long)i * 4, &x) ||
          !ii_read_i32(machine, b + (unsigned long long)i * 4, &y)) {
        return 0;
      }
      sum += (long long)x * (long long)y;
    }
    machine->fuel -= n;
    IRInterpValue out = ii_int_value(ii_as_int(&acc) + sum);
    return ii_store_dest(machine, frame, &insn->dest, &out);
  }

  case IR_OP_SIMD_SCALE_I32:
  case IR_OP_SIMD_CLAMP_I32:
  case IR_OP_SIMD_REVERSE_COPY_I32: {
    unsigned long long src, dst;
    long long n;
    IRInterpValue acc;
    if (insn->argument_count < 1 ||
        !ii_fetch_addr(machine, frame, &insn->lhs, &src) ||
        !ii_fetch_addr(machine, frame, &insn->rhs, &dst) ||
        !ii_fetch_int(machine, frame, &insn->arguments[0], &n) ||
        !ii_fetch(machine, frame, &insn->dest, &acc)) {
      return 0;
    }
    long long p1 = 0, p2 = 0;
    if (insn->op != IR_OP_SIMD_REVERSE_COPY_I32) {
      if (insn->argument_count < 3 ||
          !ii_fetch_int(machine, frame, &insn->arguments[1], &p1) ||
          !ii_fetch_int(machine, frame, &insn->arguments[2], &p2)) {
        return 0;
      }
    }
    long long sum = 0;
    for (long long i = 0; i < n; i++) {
      long long src_index = insn->op == IR_OP_SIMD_REVERSE_COPY_I32 ? n - 1 - i : i;
      int v;
      if (!ii_read_i32(machine, src + (unsigned long long)src_index * 4, &v)) {
        return 0;
      }
      int outv;
      if (insn->op == IR_OP_SIMD_SCALE_I32) {
        outv = (int)((unsigned int)v * (unsigned int)(int)p1 +
                     (unsigned int)(int)p2);
      } else if (insn->op == IR_OP_SIMD_CLAMP_I32) {
        int lo = (int)p1, hi = (int)p2;
        outv = v < lo ? lo : v > hi ? hi : v;
      } else {
        outv = v;
      }
      if (!ii_write_i32(machine, dst + (unsigned long long)i * 4, outv)) {
        return 0;
      }
      sum += outv;
    }
    machine->fuel -= n;
    IRInterpValue out = ii_int_value(ii_as_int(&acc) + sum);
    return ii_store_dest(machine, frame, &insn->dest, &out);
  }

  case IR_OP_LOWER_BOUND_I32: {
    unsigned long long base;
    long long n, key, lo0;
    if (insn->argument_count < 1 ||
        !ii_fetch_addr(machine, frame, &insn->lhs, &base) ||
        !ii_fetch_int(machine, frame, &insn->rhs, &n) ||
        !ii_fetch_int(machine, frame, &insn->arguments[0], &key) ||
        !ii_fetch_int(machine, frame, &insn->dest, &lo0)) {
      return 0;
    }
    /* dest is in/out: codegen seeds the running lo from dest's prior value
     * (the recognizer guarantees the source loop starts it at 0). Reading 0
     * here regardless would hide a transform that deletes the init. */
    long long lo = lo0, hi = n;
    while (lo < hi) {
      long long mid = lo + (hi - lo) / 2;
      int v;
      if (!ii_read_i32(machine, base + (unsigned long long)mid * 4, &v)) {
        return 0;
      }
      if ((long long)v < key) {
        lo = mid + 1;
      } else {
        hi = mid;
      }
      machine->fuel--;
    }
    IRInterpValue out = ii_int_value(lo);
    return ii_store_dest(machine, frame, &insn->dest, &out);
  }

  case IR_OP_SIMD_MINMAX_I32: {
    unsigned long long base;
    long long n;
    IRInterpValue minv, maxv;
    if (insn->argument_count < 1 ||
        !ii_fetch_addr(machine, frame, &insn->lhs, &base) ||
        !ii_fetch_int(machine, frame, &insn->rhs, &n) ||
        !ii_fetch(machine, frame, &insn->dest, &minv) ||
        !ii_fetch(machine, frame, &insn->arguments[0], &maxv)) {
      return 0;
    }
    long long mn = ii_as_int(&minv), mx = ii_as_int(&maxv);
    for (long long i = 1; i < n; i++) {
      int v;
      if (!ii_read_i32(machine, base + (unsigned long long)i * 4, &v)) {
        return 0;
      }
      if (v < mn) mn = v;
      if (v > mx) mx = v;
    }
    machine->fuel -= n;
    IRInterpValue out_min = ii_int_value(mn), out_max = ii_int_value(mx);
    return ii_store_dest(machine, frame, &insn->dest, &out_min) &&
           ii_store_dest(machine, frame, &insn->arguments[0], &out_max);
  }

  case IR_OP_SIMD_SUM_F64:
  case IR_OP_SIMD_SUM_F32: {
    unsigned long long base;
    long long n;
    IRInterpValue acc;
    if (!ii_fetch_addr(machine, frame, &insn->lhs, &base) ||
        !ii_fetch_int(machine, frame, &insn->rhs, &n) ||
        !ii_fetch(machine, frame, &insn->dest, &acc)) {
      return 0;
    }
    double sum = ii_as_float(&acc);
    for (long long i = 0; i < n; i++) {
      if (insn->op == IR_OP_SIMD_SUM_F64) {
        double v;
        if (!ii_read_f64(machine, base + (unsigned long long)i * 8, &v)) {
          return 0;
        }
        sum += v;
      } else {
        float v;
        if (!ii_read_f32(machine, base + (unsigned long long)i * 4, &v)) {
          return 0;
        }
        sum = (double)(float)((float)sum + v);
      }
    }
    machine->fuel -= n;
    IRInterpValue out = ii_float_value(sum);
    return ii_store_dest(machine, frame, &insn->dest, &out);
  }

  case IR_OP_SIMD_DOT_F64:
  case IR_OP_SIMD_DOT_F32: {
    unsigned long long a, b;
    long long n;
    IRInterpValue acc;
    if (insn->argument_count < 1 ||
        !ii_fetch_addr(machine, frame, &insn->lhs, &a) ||
        !ii_fetch_addr(machine, frame, &insn->rhs, &b) ||
        !ii_fetch_int(machine, frame, &insn->arguments[0], &n) ||
        !ii_fetch(machine, frame, &insn->dest, &acc)) {
      return 0;
    }
    double sum = ii_as_float(&acc);
    for (long long i = 0; i < n; i++) {
      if (insn->op == IR_OP_SIMD_DOT_F64) {
        double x, y;
        if (!ii_read_f64(machine, a + (unsigned long long)i * 8, &x) ||
            !ii_read_f64(machine, b + (unsigned long long)i * 8, &y)) {
          return 0;
        }
        sum += x * y;
      } else {
        float x, y;
        if (!ii_read_f32(machine, a + (unsigned long long)i * 4, &x) ||
            !ii_read_f32(machine, b + (unsigned long long)i * 4, &y)) {
          return 0;
        }
        sum = (double)(float)((float)sum + x * y);
      }
    }
    machine->fuel -= n;
    IRInterpValue out = ii_float_value(sum);
    return ii_store_dest(machine, frame, &insn->dest, &out);
  }

  case IR_OP_SIMD_AFFINE_MAP_F64:
  case IR_OP_SIMD_AFFINE_MAP_F32: {
    unsigned long long src, dst;
    long long n;
    if (insn->argument_count < 4 ||
        !ii_fetch_addr(machine, frame, &insn->lhs, &src) ||
        !ii_fetch_addr(machine, frame, &insn->rhs, &dst) ||
        !ii_fetch_int(machine, frame, &insn->arguments[0], &n)) {
      return 0;
    }
    IRInterpValue va, vb, vc;
    if (!ii_fetch(machine, frame, &insn->arguments[1], &va) ||
        !ii_fetch(machine, frame, &insn->arguments[2], &vb) ||
        !ii_fetch(machine, frame, &insn->arguments[3], &vc)) {
      return 0;
    }
    double ka = ii_as_float(&va), kb = ii_as_float(&vb), kc = ii_as_float(&vc);
    for (long long i = 0; i < n; i++) {
      if (insn->op == IR_OP_SIMD_AFFINE_MAP_F64) {
        double x, y;
        if (!ii_read_f64(machine, src + (unsigned long long)i * 8, &x) ||
            !ii_read_f64(machine, dst + (unsigned long long)i * 8, &y)) {
          return 0;
        }
        if (!ii_write_f64(machine, dst + (unsigned long long)i * 8,
                          ka * x + kb * y + kc)) {
          return 0;
        }
      } else {
        float x, y;
        if (!ii_read_f32(machine, src + (unsigned long long)i * 4, &x) ||
            !ii_read_f32(machine, dst + (unsigned long long)i * 4, &y)) {
          return 0;
        }
        float r = (float)ka * x + (float)kb * y + (float)kc;
        if (!ii_write_f32(machine, dst + (unsigned long long)i * 4, r)) {
          return 0;
        }
      }
    }
    machine->fuel -= n;
    return 1;
  }

  case IR_OP_SIMD_EXP_F32:
  case IR_OP_SIMD_SILU_F32: {
    unsigned long long out_base, g_base = 0, u_base = 0;
    long long n;
    if (insn->argument_count < 1 ||
        !ii_fetch_addr(machine, frame, &insn->dest, &out_base) ||
        !ii_fetch_int(machine, frame, &insn->arguments[0], &n)) {
      return 0;
    }
    int has_mul = 0;
    if (insn->op == IR_OP_SIMD_SILU_F32) {
      if (!ii_fetch_addr(machine, frame, &insn->lhs, &g_base)) {
        return 0;
      }
      if (insn->rhs.kind == IR_OPERAND_STRING &&
          (!insn->rhs.name || insn->rhs.name[0] == '\0')) {
        has_mul = 0;
      } else if (insn->rhs.kind == IR_OPERAND_NONE) {
        has_mul = 0;
      } else {
        if (!ii_fetch_addr(machine, frame, &insn->rhs, &u_base)) {
          return 0;
        }
        has_mul = 1;
      }
    }
    for (long long i = 0; i < n; i++) {
      if (insn->op == IR_OP_SIMD_EXP_F32) {
        float v;
        if (!ii_read_f32(machine, out_base + (unsigned long long)i * 4, &v)) {
          return 0;
        }
        if (!ii_write_f32(machine, out_base + (unsigned long long)i * 4,
                          expf(v))) {
          return 0;
        }
      } else {
        float g;
        if (!ii_read_f32(machine, g_base + (unsigned long long)i * 4, &g)) {
          return 0;
        }
        float silu = g / (1.0f + expf(-g));
        float r = silu;
        if (has_mul) {
          float u;
          if (!ii_read_f32(machine, u_base + (unsigned long long)i * 4, &u)) {
            return 0;
          }
          r = silu * u;
        }
        if (!ii_write_f32(machine, out_base + (unsigned long long)i * 4, r)) {
          return 0;
        }
      }
    }
    machine->fuel -= n;
    return 1;
  }

  case IR_OP_SIMD_I2F_REDUCE_F64: {
    if (insn->argument_count < 1) {
      ii_fail(machine, IR_INTERP_UNSUPPORTED, "i2f_reduce arity");
      return 0;
    }
    long long trip = insn->arguments[0].int_value;
    IRInterpValue acc;
    if (!ii_fetch(machine, frame, &insn->dest, &acc)) {
      return 0;
    }
    long long sum = ii_as_int(&acc);
    for (long long i = 0; i < trip; i++) {
      double x = (double)i;
      for (size_t s = 1; s + 1 < insn->argument_count; s += 2) {
        long long code = insn->arguments[s].int_value;
        double k = insn->arguments[s + 1].float_value;
        switch (code) {
        case 0: x = x * k; break; /* I2F_STEP_MUL */
        case 1: x = x + k; break; /* I2F_STEP_ADD */
        case 2: x = x - k; break; /* I2F_STEP_SUBR */
        case 3: x = k - x; break; /* I2F_STEP_SUBL */
        case 4: x = x / k; break; /* I2F_STEP_DIVR */
        default:
          ii_fail(machine, IR_INTERP_UNSUPPORTED, "i2f step code");
          return 0;
        }
      }
      sum += (long long)x; /* trunc toward zero; range proven by the pass */
    }
    machine->fuel -= trip;
    IRInterpValue out = ii_int_value(sum);
    return ii_store_dest(machine, frame, &insn->dest, &out);
  }

  case IR_OP_SIMD_LCG_U32: {
    if (insn->argument_count < 3) {
      ii_fail(machine, IR_INTERP_UNSUPPORTED, "lcg arity");
      return 0;
    }
    long long n;
    IRInterpValue state_v, acc;
    if (!ii_fetch_int(machine, frame, &insn->lhs, &n) ||
        !ii_fetch(machine, frame, &insn->rhs, &state_v) ||
        !ii_fetch(machine, frame, &insn->dest, &acc)) {
      return 0;
    }
    unsigned int state = (unsigned int)ii_as_int(&state_v);
    unsigned int A = (unsigned int)insn->arguments[0].int_value;
    unsigned int C = (unsigned int)insn->arguments[1].int_value;
    unsigned int mask = (unsigned int)insn->arguments[2].int_value;
    long long sum = ii_as_int(&acc);
    for (long long i = 0; i < n; i++) {
      state = state * A + C;
      sum += (long long)(state & mask);
    }
    machine->fuel -= n;
    IRInterpValue out_sum = ii_int_value(sum);
    IRInterpValue out_state = ii_int_value((long long)state);
    return ii_store_dest(machine, frame, &insn->dest, &out_sum) &&
           ii_store_dest(machine, frame, &insn->rhs, &out_state);
  }

  case IR_OP_SIMD_FIND: {
    if (insn->argument_count < 4) {
      ii_fail(machine, IR_INTERP_UNSUPPORTED, "simd_find arity");
      return 0;
    }
    long long n;
    unsigned long long a_base;
    if (!ii_fetch_int(machine, frame, &insn->lhs, &n) ||
        !ii_fetch_addr(machine, frame, &insn->rhs, &a_base)) {
      return 0;
    }
    long long pred = insn->arguments[0].int_value;
    long long elem_kind = insn->arguments[1].int_value;
    long long rhs_kind = insn->arguments[2].int_value;
    long long rhs_scalar = 0;
    unsigned long long b_base = 0;
    if (rhs_kind == 2) {
      if (!ii_fetch_addr(machine, frame, &insn->arguments[3], &b_base)) {
        return 0;
      }
    } else if (!ii_fetch_int(machine, frame, &insn->arguments[3], &rhs_scalar)) {
      return 0;
    }
    long long hit = n;
    for (long long i = 0; i < n; i++) {
      long long av, bv;
      if (elem_kind == 0) {
        int v;
        if (!ii_read_i32(machine, a_base + (unsigned long long)i * 4, &v)) {
          return 0;
        }
        av = v;
      } else {
        unsigned long long v;
        if (!ii_mem_read(machine, a_base + (unsigned long long)i, 1, &v)) {
          return 0;
        }
        av = (long long)v;
      }
      if (rhs_kind == 2) {
        if (elem_kind == 0) {
          int v;
          if (!ii_read_i32(machine, b_base + (unsigned long long)i * 4, &v)) {
            return 0;
          }
          bv = v;
        } else {
          unsigned long long v;
          if (!ii_mem_read(machine, b_base + (unsigned long long)i, 1, &v)) {
            return 0;
          }
          bv = (long long)v;
        }
      } else {
        bv = rhs_scalar;
      }
      int match;
      switch (pred) {
      case 0: match = av == bv; break;
      case 1: match = av != bv; break;
      case 2: match = av < bv; break;
      case 3: match = av > bv; break;
      case 4: match = av <= bv; break;
      case 5: match = av >= bv; break;
      default:
        ii_fail(machine, IR_INTERP_UNSUPPORTED, "simd_find predicate");
        return 0;
      }
      if (match) {
        hit = i;
        break;
      }
    }
    machine->fuel -= n;
    IRInterpValue out = ii_int_value(hit);
    return ii_store_dest(machine, frame, &insn->dest, &out);
  }

  case IR_OP_SIMD_VLOOP_F64:
  case IR_OP_SIMD_VLOOP_I32: {
    /* Replay the serialized straight-line DAG per element (see ir.h). */
    if (insn->argument_count < 7) {
      ii_fail(machine, IR_INTERP_UNSUPPORTED, "vloop header");
      return 0;
    }
    long long reduce_op = insn->arguments[0].int_value;
    long long n_arrays = insn->arguments[1].int_value;
    long long n_nodes = insn->arguments[2].int_value;
    long long root = insn->arguments[3].int_value;
    long long n_consts = insn->arguments[4].int_value;
    long long n_scalars = insn->arguments[5].int_value;
    size_t need = (size_t)(7 + n_arrays + n_scalars + n_nodes * 3 + n_consts);
    if (n_arrays < 0 || n_nodes <= 0 || n_nodes > 64 || n_consts < 0 ||
        n_scalars < 0 || root < 0 || root >= n_nodes ||
        insn->argument_count < need) {
      ii_fail(machine, IR_INTERP_UNSUPPORTED, "vloop layout");
      return 0;
    }
    int is_int = insn->op == IR_OP_SIMD_VLOOP_I32;
    int is_f32 = !is_int && insn->float_bits == 32;
    long long elem_size = is_int || is_f32 ? 4 : 8;

    unsigned long long arrays[32];
    double scalars_f[16];
    long long scalars_i[16];
    double consts_f[32];
    long long consts_i[32];
    if (n_arrays > 32 || n_scalars > 16 || n_consts > 32) {
      ii_fail(machine, IR_INTERP_UNSUPPORTED, "vloop widths");
      return 0;
    }
    size_t at = 7;
    for (long long k = 0; k < n_arrays; k++) {
      if (!ii_fetch_addr(machine, frame, &insn->arguments[at++], &arrays[k])) {
        return 0;
      }
    }
    for (long long k = 0; k < n_scalars; k++) {
      IRInterpValue v;
      if (!ii_fetch(machine, frame, &insn->arguments[at++], &v)) {
        return 0;
      }
      scalars_f[k] = ii_as_float(&v);
      scalars_i[k] = ii_as_int(&v);
    }
    size_t nodes_at = at;
    at += (size_t)(n_nodes * 3);
    for (long long k = 0; k < n_consts; k++) {
      const IROperand *c = &insn->arguments[at++];
      consts_f[k] = c->kind == IR_OPERAND_FLOAT ? c->float_value
                                                : (double)c->int_value;
      consts_i[k] = c->kind == IR_OPERAND_FLOAT ? (long long)c->float_value
                                                : c->int_value;
    }

    long long trip;
    if (!ii_fetch_int(machine, frame, &insn->lhs, &trip)) {
      return 0;
    }
    unsigned long long dest_base = 0;
    IRInterpValue acc;
    if (reduce_op == 1) {
      if (!ii_fetch(machine, frame, &insn->dest, &acc)) {
        return 0;
      }
    } else {
      if (!ii_fetch_addr(machine, frame, &insn->dest, &dest_base)) {
        return 0;
      }
    }

    double acc_f = reduce_op == 1 ? ii_as_float(&acc) : 0.0;
    long long acc_i = reduce_op == 1 ? ii_as_int(&acc) : 0;

    double vals_f[64];
    long long vals_i[64];
    for (long long i = 0; i < trip; i++) {
      for (long long node = 0; node < n_nodes; node++) {
        long long tag = insn->arguments[nodes_at + (size_t)node * 3].int_value;
        long long op0 = insn->arguments[nodes_at + (size_t)node * 3 + 1].int_value;
        long long op1 = insn->arguments[nodes_at + (size_t)node * 3 + 2].int_value;
        if (is_int) {
          long long v = 0;
          switch (tag) {
          case 0: { /* LOAD */
            if (op0 < 0 || op0 >= n_arrays) { ii_fail(machine, IR_INTERP_UNSUPPORTED, "vloop load idx"); return 0; }
            int e;
            if (!ii_read_i32(machine, arrays[op0] + (unsigned long long)i * 4, &e)) return 0;
            v = e;
            break;
          }
          case 1: v = i; break;
          case 2: v = op0 >= 0 && op0 < n_consts ? consts_i[op0] : 0; break;
          case 3: v = (long long)(int)((unsigned int)(int)vals_i[op0] + (unsigned int)(int)vals_i[op1]); break;
          case 4: v = (long long)(int)((unsigned int)(int)vals_i[op0] - (unsigned int)(int)vals_i[op1]); break;
          case 5: v = (long long)(int)((unsigned int)(int)vals_i[op0] * (unsigned int)(int)vals_i[op1]); break;
          case 7: v = op0 >= 0 && op0 < n_scalars ? scalars_i[op0] : 0; break;
          case 8: v = (long long)(int)((int)vals_i[op0] & (int)vals_i[op1]); break;
          case 9: v = (long long)(int)((int)vals_i[op0] | (int)vals_i[op1]); break;
          case 10: v = (long long)(int)((int)vals_i[op0] ^ (int)vals_i[op1]); break;
          case 11: v = (long long)(int)((unsigned int)(int)vals_i[op0] << (op1 & 31)); break;
          default:
            ii_fail(machine, IR_INTERP_UNSUPPORTED, "vloop int node tag");
            return 0;
          }
          vals_i[node] = v;
        } else if (is_f32) {
          float v = 0;
          switch (tag) {
          case 0: {
            if (op0 < 0 || op0 >= n_arrays) { ii_fail(machine, IR_INTERP_UNSUPPORTED, "vloop load idx"); return 0; }
            float e;
            if (!ii_read_f32(machine, arrays[op0] + (unsigned long long)i * 4, &e)) return 0;
            v = e;
            break;
          }
          case 1: v = (float)i; break;
          case 2: v = op0 >= 0 && op0 < n_consts ? (float)consts_f[op0] : 0; break;
          case 3: v = (float)vals_f[op0] + (float)vals_f[op1]; break;
          case 4: v = (float)vals_f[op0] - (float)vals_f[op1]; break;
          case 5: v = (float)vals_f[op0] * (float)vals_f[op1]; break;
          case 6: v = (float)vals_f[op0] / (float)vals_f[op1]; break;
          case 7: v = op0 >= 0 && op0 < n_scalars ? (float)scalars_f[op0] : 0; break;
          default:
            ii_fail(machine, IR_INTERP_UNSUPPORTED, "vloop f32 node tag");
            return 0;
          }
          vals_f[node] = (double)v;
        } else {
          double v = 0;
          switch (tag) {
          case 0: {
            if (op0 < 0 || op0 >= n_arrays) { ii_fail(machine, IR_INTERP_UNSUPPORTED, "vloop load idx"); return 0; }
            if (!ii_read_f64(machine, arrays[op0] + (unsigned long long)i * 8, &v)) return 0;
            break;
          }
          case 1: v = (double)i; break;
          case 2: v = op0 >= 0 && op0 < n_consts ? consts_f[op0] : 0; break;
          case 3: v = vals_f[op0] + vals_f[op1]; break;
          case 4: v = vals_f[op0] - vals_f[op1]; break;
          case 5: v = vals_f[op0] * vals_f[op1]; break;
          case 6: v = vals_f[op0] / vals_f[op1]; break;
          case 7: v = op0 >= 0 && op0 < n_scalars ? scalars_f[op0] : 0; break;
          default:
            ii_fail(machine, IR_INTERP_UNSUPPORTED, "vloop f64 node tag");
            return 0;
          }
          vals_f[node] = v;
        }
      }
      if (reduce_op == 1) {
        if (is_int) {
          acc_i = (long long)(int)((unsigned int)(int)acc_i +
                                   (unsigned int)(int)vals_i[root]);
        } else if (is_f32) {
          acc_f = (double)(float)((float)acc_f + (float)vals_f[root]);
        } else {
          acc_f += vals_f[root];
        }
      } else {
        unsigned long long addr =
            dest_base + (unsigned long long)i * (unsigned long long)elem_size;
        if (is_int) {
          if (!ii_write_i32(machine, addr, (int)vals_i[root])) return 0;
        } else if (is_f32) {
          if (!ii_write_f32(machine, addr, (float)vals_f[root])) return 0;
        } else {
          if (!ii_write_f64(machine, addr, vals_f[root])) return 0;
        }
      }
      machine->fuel -= n_nodes;
      if (machine->fuel < 0) {
        ii_fail(machine, IR_INTERP_FUEL, "vloop fuel");
        return 0;
      }
    }
    if (reduce_op == 1) {
      IRInterpValue out = is_int ? ii_int_value(acc_i) : ii_float_value(acc_f);
      return ii_store_dest(machine, frame, &insn->dest, &out);
    }
    return 1;
  }

  default:
    ii_fail(machine, IR_INTERP_UNSUPPORTED, ir_opcode_name(insn->op));
    return 0;
  }
}

/* ---------------- main execution loop ---------------- */

static int ii_exec_function(IRInterpMachine *machine, IRFunction *fn,
                            const IRInterpValue *args, size_t arg_count,
                            IRInterpValue *result) {
  if (machine->depth >= II_MAX_DEPTH) {
    ii_fail(machine, IR_INTERP_DEPTH, fn->name ? fn->name : "?");
    return 0;
  }
  machine->depth++;

  IIFrame frame;
  memset(&frame, 0, sizeof(frame));
  frame.fn = fn;

  int ok = 0;

  /* Bind parameters. */
  for (size_t i = 0; i < fn->parameter_count && i < arg_count; i++) {
    IIVar *var = ii_env_upsert(&frame.env, fn->parameter_names[i]);
    if (!var) {
      ii_fail(machine, IR_INTERP_TRAP, "out of memory");
      goto done;
    }
    var->value = args[i];
  }

  /* Pre-scan: label table + address-taken local set. */
  size_t label_capacity = 8;
  frame.labels = (IILabel *)malloc(label_capacity * sizeof(IILabel));
  if (!frame.labels) {
    ii_fail(machine, IR_INTERP_TRAP, "out of memory");
    goto done;
  }
  for (size_t i = 0; i < fn->instruction_count; i++) {
    const IRInstruction *insn = &fn->instructions[i];
    if (insn->op == IR_OP_LABEL && insn->text) {
      if (frame.label_count >= label_capacity) {
        label_capacity *= 2;
        IILabel *grown =
            (IILabel *)realloc(frame.labels, label_capacity * sizeof(IILabel));
        if (!grown) {
          ii_fail(machine, IR_INTERP_TRAP, "out of memory");
          goto done;
        }
        frame.labels = grown;
      }
      frame.labels[frame.label_count].label = insn->text;
      frame.labels[frame.label_count].index = i;
      frame.label_count++;
    }
  }

  long long *exec_counts = ii_counts_for(machine, fn);

  size_t pc = 0;
  while (pc < fn->instruction_count) {
    if (--machine->fuel < 0) {
      ii_fail(machine, IR_INTERP_FUEL, fn->name ? fn->name : "?");
      goto done;
    }
    const IRInstruction *insn = &fn->instructions[pc];
    size_t executed_pc = pc;

    switch (insn->op) {
    case IR_OP_NOP:
    case IR_OP_LABEL:
      pc++;
      break;

    case IR_OP_JUMP: {
      const IILabel *label = insn->text ? ii_find_label(&frame, insn->text) : NULL;
      if (!label) {
        ii_fail(machine, IR_INTERP_TRAP, "jump to unknown label");
        goto done;
      }
      pc = label->index;
      break;
    }

    case IR_OP_BRANCH_ZERO: {
      IRInterpValue cond;
      if (!ii_fetch(machine, &frame, &insn->lhs, &cond)) {
        goto done;
      }
      long long v = cond.is_float ? (cond.f != 0.0) : cond.i;
      if (v == 0) {
        const IILabel *label = insn->text ? ii_find_label(&frame, insn->text) : NULL;
        if (!label) {
          ii_fail(machine, IR_INTERP_TRAP, "branch to unknown label");
          goto done;
        }
        pc = label->index;
      } else {
        pc++;
      }
      break;
    }

    case IR_OP_BRANCH_EQ: {
      IRInterpValue a, b;
      if (!ii_fetch(machine, &frame, &insn->lhs, &a) ||
          !ii_fetch(machine, &frame, &insn->rhs, &b)) {
        goto done;
      }
      int equal;
      if (a.is_float || b.is_float) {
        equal = ii_as_float(&a) == ii_as_float(&b);
      } else {
        equal = a.i == b.i;
      }
      if (equal) {
        const IILabel *label = insn->text ? ii_find_label(&frame, insn->text) : NULL;
        if (!label) {
          ii_fail(machine, IR_INTERP_TRAP, "branch to unknown label");
          goto done;
        }
        pc = label->index;
      } else {
        pc++;
      }
      break;
    }

    case IR_OP_DECLARE_LOCAL: {
      if (insn->dest.kind != IR_OPERAND_SYMBOL || !insn->dest.name) {
        ii_fail(machine, IR_INTERP_UNSUPPORTED, "local declaration form");
        goto done;
      }
      int elem_size = 8, is_float = 0, is_unsigned = 0;
      long long count = 1;
      if (!ii_parse_local_type(insn->text, &elem_size, &count, &is_float,
                               &is_unsigned)) {
        char what[96];
        snprintf(what, sizeof(what), "local type '%s'",
                 insn->text ? insn->text : "?");
        ii_fail(machine, IR_INTERP_UNSUPPORTED, what);
        goto done;
      }
      IIVar *var = ii_env_upsert(&frame.env, insn->dest.name);
      if (!var) {
        ii_fail(machine, IR_INTERP_TRAP, "out of memory");
        goto done;
      }
      /* Arrays always get storage; scalars only when their address is
       * taken somewhere in the function (aliasing must be observable). */
      int need_slot = count > 1;
      if (!need_slot) {
        for (size_t i = 0; i < fn->instruction_count; i++) {
          const IRInstruction *scan = &fn->instructions[i];
          if (scan->op == IR_OP_ADDRESS_OF &&
              scan->lhs.kind == IR_OPERAND_SYMBOL && scan->lhs.name &&
              strcmp(scan->lhs.name, insn->dest.name) == 0) {
            need_slot = 1;
            break;
          }
        }
      }
      if (need_slot) {
        unsigned long long addr =
            ir_interp_add_buffer(machine, NULL, count * elem_size);
        if (!addr) {
          ii_fail(machine, IR_INTERP_TRAP, "local storage allocation");
          goto done;
        }
        /* Local storage is stack memory: poison it (heap `new` stays zeroed,
         * matching HEAP_ZERO_MEMORY in codegen). */
        memset(machine->buffers[machine->buffer_count - 1].data,
               II_POISON_BYTE, (size_t)(count * elem_size));
        var->value = ii_int_value((long long)addr);
        var->slotted = count == 1; /* arrays are accessed via &, not by name */
        var->slot_size = elem_size;
        var->slot_is_float = is_float;
        var->slot_is_unsigned = is_unsigned;
      } else {
        var->value = ii_poison_value();
        var->slotted = 0;
        if (is_float) {
          var->value.is_float = 1;
          var->value.f = -6510615.5; /* deterministic float poison */
          var->value.i = 0;
        }
      }
      pc++;
      break;
    }

    case IR_OP_ASSIGN: {
      IRInterpValue value;
      if (!ii_fetch(machine, &frame, &insn->lhs, &value) ||
          !ii_store_dest(machine, &frame, &insn->dest, &value)) {
        goto done;
      }
      pc++;
      break;
    }

    case IR_OP_ADDRESS_OF: {
      if (insn->lhs.kind != IR_OPERAND_SYMBOL || !insn->lhs.name) {
        ii_fail(machine, IR_INTERP_UNSUPPORTED, "address-of non-symbol");
        goto done;
      }
      IIVar *var = ii_env_find(&frame.env, insn->lhs.name);
      if (!var) {
        ii_fail(machine, IR_INTERP_UNSUPPORTED, "address-of global");
        goto done;
      }
      /* Slot-backed local or array: value.i is the base address. */
      IRInterpValue addr = ii_int_value(var->value.i);
      if (!ii_store_dest(machine, &frame, &insn->dest, &addr)) {
        goto done;
      }
      pc++;
      break;
    }

    case IR_OP_LOAD: {
      unsigned long long addr;
      long long size;
      if (!ii_fetch_addr(machine, &frame, &insn->lhs, &addr) ||
          !ii_fetch_int(machine, &frame, &insn->rhs, &size)) {
        goto done;
      }
      if (size != 1 && size != 2 && size != 4 && size != 8) {
        ii_fail(machine, IR_INTERP_UNSUPPORTED, "load size");
        goto done;
      }
      unsigned long long raw;
      if (!ii_mem_read(machine, addr, (int)size, &raw)) {
        goto done;
      }
      IRInterpValue value;
      if (insn->is_float) {
        if (size == 4) {
          float f;
          unsigned int bits = (unsigned int)raw;
          memcpy(&f, &bits, 4);
          value = ii_float_value((double)f);
        } else {
          double d;
          memcpy(&d, &raw, 8);
          value = ii_float_value(d);
        }
      } else {
        long long v = (long long)raw;
        if (size < 8 && !insn->is_unsigned) {
          int shift = 64 - (int)size * 8;
          v = (v << shift) >> shift;
        }
        value = ii_int_value(v);
      }
      if (!ii_store_dest(machine, &frame, &insn->dest, &value)) {
        goto done;
      }
      pc++;
      break;
    }

    case IR_OP_STORE: {
      unsigned long long addr;
      long long size;
      IRInterpValue value;
      if (!ii_fetch_addr(machine, &frame, &insn->dest, &addr) ||
          !ii_fetch_int(machine, &frame, &insn->rhs, &size) ||
          !ii_fetch(machine, &frame, &insn->lhs, &value)) {
        goto done;
      }
      if (size != 1 && size != 2 && size != 4 && size != 8) {
        ii_fail(machine, IR_INTERP_UNSUPPORTED, "store size");
        goto done;
      }
      unsigned long long raw;
      if (value.is_float || insn->is_float) {
        if (size == 4) {
          float f = (float)ii_as_float(&value);
          unsigned int bits;
          memcpy(&bits, &f, 4);
          raw = bits;
        } else if (size == 8) {
          double d = ii_as_float(&value);
          memcpy(&raw, &d, 8);
        } else {
          raw = (unsigned long long)ii_as_int(&value);
        }
        /* An int value stored with an int-typed instruction keeps int bits. */
        if (!value.is_float && !insn->is_float) {
          raw = (unsigned long long)value.i;
        }
      } else {
        raw = (unsigned long long)value.i;
      }
      if (!ii_mem_write(machine, addr, (int)size, raw)) {
        goto done;
      }
      pc++;
      break;
    }

    case IR_OP_BINARY: {
      IRInterpValue a, b, out;
      if (!ii_fetch(machine, &frame, &insn->lhs, &a) ||
          !ii_fetch(machine, &frame, &insn->rhs, &b) ||
          !ii_binary(machine, insn, &a, &b, &out) ||
          !ii_store_dest(machine, &frame, &insn->dest, &out)) {
        goto done;
      }
      pc++;
      break;
    }

    case IR_OP_UNARY: {
      IRInterpValue a, out;
      if (!ii_fetch(machine, &frame, &insn->lhs, &a)) {
        goto done;
      }
      const char *op = insn->text ? insn->text : "?";
      if (strcmp(op, "-") == 0) {
        if (insn->is_float || a.is_float) {
          double v = ii_as_float(&a);
          out = ii_float_value(insn->float_bits == 32 ? (double)(-(float)v) : -v);
        } else {
          out = ii_int_value((long long)(0ULL - (unsigned long long)a.i));
        }
      } else if (strcmp(op, "!") == 0) {
        out = ii_int_value(a.is_float ? a.f == 0.0 : a.i == 0);
      } else if (strcmp(op, "~") == 0) {
        out = ii_int_value(~ii_as_int(&a));
      } else {
        ii_fail(machine, IR_INTERP_UNSUPPORTED, "unary op");
        goto done;
      }
      if (!ii_store_dest(machine, &frame, &insn->dest, &out)) {
        goto done;
      }
      pc++;
      break;
    }

    case IR_OP_ROTATE_ADD: {
      /* next = a + b; a = b; b = next  (dest=next, lhs=a, rhs=b) */
      IRInterpValue a, b;
      if (!ii_fetch(machine, &frame, &insn->lhs, &a) ||
          !ii_fetch(machine, &frame, &insn->rhs, &b)) {
        goto done;
      }
      IRInterpValue next = ii_int_value(
          (long long)((unsigned long long)ii_as_int(&a) +
                      (unsigned long long)ii_as_int(&b)));
      if (!ii_store_dest(machine, &frame, &insn->dest, &next) ||
          !ii_store_dest(machine, &frame, &insn->lhs, &b) ||
          !ii_store_dest(machine, &frame, &insn->rhs, &next)) {
        goto done;
      }
      pc++;
      break;
    }

    case IR_OP_CAST: {
      IRInterpValue a, out;
      if (!ii_fetch(machine, &frame, &insn->lhs, &a) ||
          !ii_cast(machine, insn, &a, &out) ||
          !ii_store_dest(machine, &frame, &insn->dest, &out)) {
        goto done;
      }
      pc++;
      break;
    }

    case IR_OP_SELECT: {
      /* Fused form (text != NULL): cond = (lhs <cmp> arguments[1]). Plain
       * form: cond = (lhs != 0). Then dest = cond ? rhs : arguments[0]. */
      long long truth = 0;
      if (insn->text && insn->argument_count > 1) {
        IRInterpValue a, b;
        if (!ii_fetch(machine, &frame, &insn->lhs, &a) ||
            !ii_fetch(machine, &frame, &insn->arguments[1], &b)) {
          goto done;
        }
        long long x = ii_as_int(&a), y = ii_as_int(&b);
        const char *op = insn->text;
        if (insn->is_unsigned) {
          unsigned long long ux = (unsigned long long)x, uy = (unsigned long long)y;
          truth = strcmp(op, "<") == 0    ? ux < uy
                  : strcmp(op, "<=") == 0 ? ux <= uy
                  : strcmp(op, ">") == 0  ? ux > uy
                  : strcmp(op, ">=") == 0 ? ux >= uy
                  : strcmp(op, "==") == 0 ? ux == uy
                                          : ux != uy;
        } else {
          truth = strcmp(op, "<") == 0    ? x < y
                  : strcmp(op, "<=") == 0 ? x <= y
                  : strcmp(op, ">") == 0  ? x > y
                  : strcmp(op, ">=") == 0 ? x >= y
                  : strcmp(op, "==") == 0 ? x == y
                                          : x != y;
        }
      } else {
        IRInterpValue cond;
        if (!ii_fetch(machine, &frame, &insn->lhs, &cond)) {
          goto done;
        }
        truth = ii_as_int(&cond) != 0;
      }
      IRInterpValue out;
      const IROperand *chosen =
          truth ? &insn->rhs
                : (insn->argument_count > 0 ? &insn->arguments[0] : NULL);
      if (!chosen || !ii_fetch(machine, &frame, chosen, &out) ||
          !ii_store_dest(machine, &frame, &insn->dest, &out)) {
        goto done;
      }
      pc++;
      break;
    }

    case IR_OP_NEW: {
      long long size = 8;
      if (insn->rhs.kind == IR_OPERAND_INT && insn->rhs.int_value > 0) {
        size = insn->rhs.int_value;
      } else if (insn->rhs.kind != IR_OPERAND_NONE) {
        if (!ii_fetch_int(machine, &frame, &insn->rhs, &size)) {
          goto done;
        }
        if (size <= 0) {
          size = 8;
        }
      }
      unsigned long long addr = ir_interp_add_buffer(machine, NULL, size);
      if (!addr) {
        ii_fail(machine, IR_INTERP_TRAP, "new allocation");
        goto done;
      }
      machine->buffers[machine->buffer_count - 1].alloc_line =
          insn->location.line;
      IRInterpValue value = ii_int_value((long long)addr);
      if (!ii_store_dest(machine, &frame, &insn->dest, &value)) {
        goto done;
      }
      pc++;
      break;
    }

    case IR_OP_CALL: {
      if (!insn->text) {
        ii_fail(machine, IR_INTERP_UNSUPPORTED, "call without callee");
        goto done;
      }
      IRInterpValue call_args[16];
      size_t call_arg_count = insn->argument_count;
      if (call_arg_count > 16) {
        ii_fail(machine, IR_INTERP_UNSUPPORTED, "call arity > 16");
        goto done;
      }
      for (size_t i = 0; i < call_arg_count; i++) {
        const IROperand *arg = &insn->arguments[i];
        if (arg->kind == IR_OPERAND_STRING) {
          /* Trace-visible token for a string literal argument. */
          call_args[i] = ii_int_value(
              (long long)(ii_hash(arg->name ? arg->name : "") | 1ULL));
          continue;
        }
        if (!ii_fetch(machine, &frame, arg, &call_args[i])) {
          goto done;
        }
      }
      IRFunction *callee = ii_find_function(machine, insn->text);
      IRInterpValue call_result = ii_int_value(0);
      if (callee && callee->instruction_count > 0) {
        if (!ii_exec_function(machine, callee, call_args, call_arg_count,
                              &call_result)) {
          goto done;
        }
      } else {
        machine->current_call_loc = insn->location;
        size_t buffers_before = machine->buffer_count;
        int handled =
            ii_extern_call(machine, insn->text, call_args, call_arg_count,
                           &call_result);
        /* Attribute any heap allocation the extern model made (malloc,
         * calloc, realloc) to this call site for leak reporting. */
        for (size_t bi = buffers_before; bi < machine->buffer_count; bi++) {
          machine->buffers[bi].alloc_line = insn->location.line;
        }
        if (handled < 0 || machine->status != IR_INTERP_OK) {
          if (machine->status == IR_INTERP_OK) {
            ii_fail(machine, IR_INTERP_TRAP, "extern call trap");
          }
          goto done;
        }
      }
      if (!ii_store_dest(machine, &frame, &insn->dest, &call_result)) {
        goto done;
      }
      pc++;
      break;
    }

    case IR_OP_RETURN: {
      IRInterpValue value = ii_int_value(0);
      if (insn->lhs.kind != IR_OPERAND_NONE &&
          !ii_fetch(machine, &frame, &insn->lhs, &value)) {
        goto done;
      }
      *result = value;
      ok = 1;
      goto done;
    }

    case IR_OP_CALL_INDIRECT:
      ii_fail(machine, IR_INTERP_UNSUPPORTED, "call_indirect");
      goto done;
    case IR_OP_INLINE_ASM:
      ii_fail(machine, IR_INTERP_UNSUPPORTED, "inline_asm");
      goto done;

    default:
      if (!ii_exec_simd(machine, &frame, insn)) {
        goto done;
      }
      if (machine->fuel < 0) {
        ii_fail(machine, IR_INTERP_FUEL, fn->name ? fn->name : "?");
        goto done;
      }
      pc++;
      break;
    }

    if (exec_counts) {
      exec_counts[executed_pc]++;
    }

    /* Value tracing: report the executed instruction's named result. */
    if (machine->value_hook && frame.fn == machine->value_hook_fn &&
        insn->location.line > 0 &&
        (insn->dest.kind == IR_OPERAND_TEMP ||
         insn->dest.kind == IR_OPERAND_SYMBOL) &&
        insn->dest.name) {
      switch (insn->op) {
      case IR_OP_ASSIGN:
      case IR_OP_BINARY:
      case IR_OP_UNARY:
      case IR_OP_CAST:
      case IR_OP_LOAD:
      case IR_OP_CALL:
      case IR_OP_ROTATE_ADD:
      case IR_OP_NEW: {
        IIVar *var = ii_env_find(&frame.env, insn->dest.name);
        if (!var && insn->dest.kind == IR_OPERAND_SYMBOL) {
          var = ii_env_find(&machine->globals, insn->dest.name);
        }
        IRInterpValue value;
        if (var && ii_var_read(machine, var, &value)) {
          machine->value_hook(machine->value_hook_ctx, insn->location.line,
                              insn->dest.name, value);
        }
        break;
      }
      default:
        break;
      }
    }
  }

  /* Fell off the end: void return. */
  *result = ii_int_value(0);
  ok = 1;

done:
  free(frame.labels);
  ii_env_free(&frame.env);
  machine->depth--;
  return ok;
}

IRInterpStatus ir_interp_run(IRInterpMachine *machine, IRFunction *function,
                             const IRInterpValue *args, size_t arg_count,
                             IRInterpValue *result, long long fuel) {
  if (!machine || !function || !result) {
    return IR_INTERP_UNSUPPORTED;
  }
  machine->status = IR_INTERP_OK;
  machine->detail[0] = '\0';
  machine->fuel = fuel;
  machine->depth = 0;
  machine->assert_failed = 0;
  IRInterpValue local_result = ii_int_value(0);
  int ok = ii_exec_function(machine, function, args, arg_count, &local_result);
  if (ok && machine->status == IR_INTERP_OK) {
    *result = local_result;
    return IR_INTERP_OK;
  }
  return machine->status == IR_INTERP_OK ? IR_INTERP_TRAP : machine->status;
}

/* ---------------- observation accessors ---------------- */

size_t ir_interp_buffer_count(const IRInterpMachine *machine) {
  return machine ? machine->buffer_count : 0;
}

const unsigned char *ir_interp_buffer_data(const IRInterpMachine *machine,
                                           size_t index, long long *size) {
  if (!machine || index >= machine->buffer_count) {
    return NULL;
  }
  if (size) {
    *size = machine->buffers[index].size;
  }
  return machine->buffers[index].data;
}

size_t ir_interp_extern_trace_count(const IRInterpMachine *machine) {
  return machine ? machine->trace_count : 0;
}

const IRInterpExternCall *ir_interp_extern_trace(const IRInterpMachine *machine,
                                                 size_t index) {
  if (!machine || index >= machine->trace_count) {
    return NULL;
  }
  return &machine->trace[index];
}

size_t ir_interp_global_count(const IRInterpMachine *machine) {
  return machine ? machine->globals.capacity : 0;
}

const char *ir_interp_global_name(const IRInterpMachine *machine,
                                  size_t index) {
  if (!machine || index >= machine->globals.capacity) {
    return NULL;
  }
  return machine->globals.vars[index].key;
}

IRInterpValue ir_interp_global_value(const IRInterpMachine *machine,
                                     size_t index) {
  IRInterpValue zero = {0, 0, 0};
  if (!machine || index >= machine->globals.capacity ||
      !machine->globals.vars[index].key) {
    return zero;
  }
  return machine->globals.vars[index].value;
}

const char *ir_interp_status_detail(const IRInterpMachine *machine) {
  return machine ? machine->detail : "";
}

int ir_interp_assert_info(const IRInterpMachine *machine, size_t *line,
                          size_t *column, IRInterpValue *left,
                          IRInterpValue *right, int *is_eq) {
  if (!machine || !machine->assert_failed) {
    return 0;
  }
  if (line) *line = machine->assert_line;
  if (column) *column = machine->assert_column;
  if (left) *left = machine->assert_left;
  if (right) *right = machine->assert_right;
  if (is_eq) *is_eq = machine->assert_is_eq;
  return 1;
}

size_t ir_interp_buffer_alloc_line(const IRInterpMachine *machine,
                                   size_t index) {
  if (!machine || index >= machine->buffer_count) {
    return 0;
  }
  return machine->buffers[index].alloc_line;
}

int ir_interp_buffer_freed(const IRInterpMachine *machine, size_t index) {
  if (!machine || index >= machine->buffer_count) {
    return 1;
  }
  return machine->buffers[index].freed;
}

void ir_interp_set_value_hook(IRInterpMachine *machine, IRInterpValueHook hook,
                              void *ctx, const IRFunction *only_in) {
  if (!machine) {
    return;
  }
  machine->value_hook = hook;
  machine->value_hook_ctx = ctx;
  machine->value_hook_fn = only_in;
}
