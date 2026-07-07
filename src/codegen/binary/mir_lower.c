#include "codegen/binary/mir.h"

#include "codegen/binary/mir_annotate.h"
#include "common.h"
#include "ir/ir_optimize.h"
#include "semantic/symbol_table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Source file a function was declared in (for the --explain focus filter). */
static const char *mir_function_filename(FunctionDeclaration *fd) {
  return (fd && fd->body) ? fd->body->location.filename : NULL;
}

/* Non-nop IR size of the function currently in the eligibility gate, captured
 * at gate entry so the dozens of bail sites can report it to --explain
 * without each threading it through (the report ranks bailed functions by
 * size -- that is where baseline codegen actually costs). */
static size_t g_mir_gate_fn_size = 0;

/* TEMPORARY instrumentation: with METTLE_MIR_TRACE set, log why a function is
 * rejected by the MIR eligibility gate, so the spill-everything-fallback work
 * list can be prioritized by real frequency. Returns 0 (ineligible). Also
 * feeds the --explain backend report (a no-op when --explain is off). */
/* getenv is slow on Windows and these are consulted per function (or per
 * bail); snapshot each knob once per process. */
static int mir_env_trace(void) {
  static int cached = -1;
  if (cached < 0) {
    cached = getenv("METTLE_MIR_TRACE") ? 1 : 0;
  }
  return cached;
}

static const char *mir_env_mir(void) {
  static const char *cached = NULL;
  static int resolved = 0;
  if (!resolved) {
    cached = getenv("METTLE_MIR");
    resolved = 1;
  }
  return cached;
}

static const char *mir_env_skipfn(void) {
  static const char *cached = NULL;
  static int resolved = 0;
  if (!resolved) {
    cached = getenv("METTLE_MIR_SKIPFN");
    resolved = 1;
  }
  return cached;
}

static int mir_trace_bail(FunctionDeclaration *fd, const char *reason) {
  if (mir_env_trace()) {
    fprintf(stderr, "MIR-BAIL\t%s\t%s\n", reason,
            (fd && fd->name) ? fd->name : "?");
  }
  if (ir_explain_enabled() && fd && fd->name) {
    ir_explain_backend_function(fd->name, mir_function_filename(fd), 0, reason,
                                g_mir_gate_fn_size);
  }
  return 0;
}

/* True if `name` resolves to a read-accessible global scalar — a value we can
 * cache in a register at function entry (used by both the eligibility gate and
 * the entry-load emitter, so they agree exactly on what counts as cacheable). */
static int mir_name_is_global_scalar(CodeGenerator *g, const char *name) {
  if (!g || !g->symbol_table || !name) {
    return 0;
  }
  Symbol *s = symbol_table_lookup(g->symbol_table, name);
  if (!s || s->kind != SYMBOL_VARIABLE || !s->scope ||
      s->scope->type != SCOPE_GLOBAL) {
    return 0;
  }
  return code_generator_binary_symbol_is_scalar_accessible(g, name);
}

/* IR -> MIR lowering for the Stage 2 scalar-integer subset, plus the
 * per-function eligibility gate and the MIR emit entry point.
 *
 * Eligible functions (see mir_function_is_eligible) are pure leaf integer code:
 * no calls, no address-of, no floats, no aggregates, <=4 GP params, and only
 * the opcodes handled below. Anything else falls back to the legacy emitter.
 * All values are computed as 64-bit; loads/stores carry their own width and
 * casts re-extend, so holding everything in 64-bit registers is exact. */

/* ---- name -> vreg map --------------------------------------------------- */

typedef struct {
  const char *name; /* borrowed (interned IR string) */
  int is_temp;      /* TEMP and SYMBOL operands are distinct namespaces: a
                       compiler temp may share its bare name with a user
                       local, and conflating them merges their storage */
  MirVregId vreg;
} MirNameEntry;

typedef struct {
  MirNameEntry *items;
  size_t count;
  size_t capacity;
  /* Open-addressing index over items (slot+1; 0 = empty). Linear name scans
   * here were a measured hotspot on large inlined functions. */
  size_t *buckets;
  size_t bucket_count;
} MirNameMap;

static void mir_name_map_destroy(MirNameMap *m) {
  free(m->items);
  free(m->buckets);
  m->items = NULL;
  m->buckets = NULL;
  m->count = m->capacity = m->bucket_count = 0;
}

static size_t mir_name_map_hash(const char *name, int is_temp) {
  size_t h = mettle_fnv1a_hash(name);
  return h ^ (is_temp ? 0x9e3779b97f4a7c15ull : 0);
}

static int mir_name_map_reindex(MirNameMap *m, size_t min_buckets) {
  size_t nb = 64;
  while (nb < min_buckets) {
    nb *= 2;
  }
  size_t *fresh = (size_t *)calloc(nb, sizeof(size_t));
  if (!fresh) {
    return 0;
  }
  for (size_t i = 0; i < m->count; i++) {
    size_t b = mir_name_map_hash(m->items[i].name, m->items[i].is_temp) &
               (nb - 1);
    while (fresh[b]) {
      b = (b + 1) & (nb - 1);
    }
    fresh[b] = i + 1;
  }
  free(m->buckets);
  m->buckets = fresh;
  m->bucket_count = nb;
  return 1;
}

static MirVregId mir_name_map_get_or_add(MirNameMap *m, MirFunction *fn,
                                         const char *name, int is_temp,
                                         MirRegClass rclass, int width) {
  if (m->bucket_count && m->count * 4 < m->bucket_count * 3) {
    size_t b = mir_name_map_hash(name, is_temp) & (m->bucket_count - 1);
    while (m->buckets[b]) {
      const MirNameEntry *e = &m->items[m->buckets[b] - 1];
      if (e->is_temp == is_temp && strcmp(e->name, name) == 0) {
        return e->vreg;
      }
      b = (b + 1) & (m->bucket_count - 1);
    }
  } else {
    for (size_t i = 0; i < m->count; i++) {
      if (m->items[i].is_temp == is_temp &&
          strcmp(m->items[i].name, name) == 0) {
        return m->items[i].vreg;
      }
    }
  }
  if (m->count >= m->capacity) {
    size_t nc = m->capacity ? m->capacity * 2 : 16;
    MirNameEntry *grown =
        (MirNameEntry *)realloc(m->items, nc * sizeof(MirNameEntry));
    if (!grown) {
      fn->has_error = 1;
      return MIR_VREG_NONE;
    }
    m->items = grown;
    m->capacity = nc;
  }
  MirVregId v = mir_new_vreg(fn, rclass, width);
  if (v == MIR_VREG_NONE) {
    return MIR_VREG_NONE;
  }
  m->items[m->count].name = name;
  m->items[m->count].is_temp = is_temp;
  m->items[m->count].vreg = v;
  m->count++;
  if ((m->count + 1) * 4 >= m->bucket_count * 3) {
    if (!mir_name_map_reindex(m, (m->count + 1) * 2)) {
      fn->has_error = 1;
      return MIR_VREG_NONE;
    }
  } else {
    size_t b = mir_name_map_hash(name, is_temp) & (m->bucket_count - 1);
    while (m->buckets[b]) {
      b = (b + 1) & (m->bucket_count - 1);
    }
    m->buckets[b] = m->count; /* slot index (count-1) + 1 */
  }
  return v;
}

/* True if symbol `name` already has a vreg binding (param/local/cached
 * global). Symbols only — temps live in a separate namespace. */
static int mir_name_map_has(const MirNameMap *m, const char *name) {
  if (m->bucket_count) {
    size_t b = mir_name_map_hash(name, 0) & (m->bucket_count - 1);
    while (m->buckets[b]) {
      const MirNameEntry *e = &m->items[m->buckets[b] - 1];
      if (!e->is_temp && strcmp(e->name, name) == 0) {
        return 1;
      }
      b = (b + 1) & (m->bucket_count - 1);
    }
    return 0;
  }
  for (size_t i = 0; i < m->count; i++) {
    if (!m->items[i].is_temp && strcmp(m->items[i].name, name) == 0) {
      return 1;
    }
  }
  return 0;
}

/* Register-promoted globals. Each referenced global scalar is loaded once at
 * entry (MIR_LOAD_GLOBAL) into a cache vreg; `all` lists every cached global and
 * `names` the subset that is written (stored back before every return). In a
 * function that makes calls, memory — not the cache vreg — is authoritative
 * across a call boundary: the written set is flushed before each call (so the
 * callee sees current values) and the full cached set is reloaded after (so we
 * observe any value the callee changed). Names are borrowed interned IR
 * strings. */
typedef struct {
  const char **names; /* written globals (write-back / flush-before-call) */
  size_t count;
  const char **all; /* every cached global (reload-after-call) */
  size_t all_count;
  const char **at;  /* address-taken globals (aliasable via &g): flush before a
                       pointer LOAD/STORE, reload after a pointer STORE, so a
                       store through the alias and a by-name access stay coherent */
  size_t at_count;
} MirGlobalWriteback;

/* ---- operand mapping ---------------------------------------------------- */

/* Map an IR operand that must resolve to a value: a float TEMP/SYMBOL -> an XMM
 * vreg, an int TEMP/SYMBOL -> a GP vreg, INT -> immediate, FLOAT -> float
 * immediate (raw IEEE bits). Sets has_error for anything outside the subset. */
static MirOperand mir_value_operand(MirFunction *fn, CodeGenerator *g,
                                    BinaryFunctionContext *ctx, MirNameMap *map,
                                    const IROperand *op) {
  switch (op->kind) {
  case IR_OPERAND_TEMP:
  case IR_OPERAND_SYMBOL: {
    int fb = code_generator_binary_operand_float_bits(g, ctx, op);
    MirRegClass rc = fb ? MIR_RC_XMM : MIR_RC_GP;
    int w = fb ? fb / 8 : 8;
    MirVregId v = mir_name_map_get_or_add(map, fn, op->name,
                                          op->kind == IR_OPERAND_TEMP, rc, w);
    return mir_op_vreg(v);
  }
  case IR_OPERAND_INT:
    return mir_op_imm(op->int_value);
  case IR_OPERAND_FLOAT: {
    int fb = op->float_bits == 32 ? 32 : 64;
    uint64_t bits;
    if (fb == 32) {
      float fv = (float)op->float_value;
      uint32_t u;
      memcpy(&u, &fv, sizeof(u));
      bits = u;
    } else {
      double dv = op->float_value;
      uint64_t u;
      memcpy(&u, &dv, sizeof(u));
      bits = u;
    }
    return mir_op_fimm(bits);
  }
  default:
    fn->has_error = 1;
    return mir_op_none();
  }
}

/* ---- compare/shift helpers ---------------------------------------------- */

/* setcc opcode (second byte) for an IR comparison operator, signed or not. */
static int mir_setcc_opcode(const char *op, int is_unsigned, unsigned char *out) {
  if (strcmp(op, "==") == 0) { *out = 0x94; return 1; }
  if (strcmp(op, "!=") == 0) { *out = 0x95; return 1; }
  if (strcmp(op, "<") == 0)  { *out = is_unsigned ? 0x92 : 0x9C; return 1; }
  if (strcmp(op, "<=") == 0) { *out = is_unsigned ? 0x96 : 0x9E; return 1; }
  if (strcmp(op, ">") == 0)  { *out = is_unsigned ? 0x97 : 0x9F; return 1; }
  if (strcmp(op, ">=") == 0) { *out = is_unsigned ? 0x93 : 0x9D; return 1; }
  return 0;
}

static int mir_is_comparison(const char *op) {
  unsigned char tmp;
  return mir_setcc_opcode(op, 0, &tmp);
}

/* jcc opcode (second byte) to take when an IR comparison is FALSE — i.e. the
 * branch a `branch_zero` of the comparison result should take. */
static int mir_false_jcc(const char *op, int is_unsigned, unsigned char *out) {
  if (strcmp(op, "==") == 0) { *out = 0x85; return 1; } /* jne */
  if (strcmp(op, "!=") == 0) { *out = 0x84; return 1; } /* je */
  if (strcmp(op, "<") == 0)  { *out = is_unsigned ? 0x83 : 0x8D; return 1; } /* jae/jge */
  if (strcmp(op, "<=") == 0) { *out = is_unsigned ? 0x87 : 0x8F; return 1; } /* ja/jg */
  if (strcmp(op, ">") == 0)  { *out = is_unsigned ? 0x86 : 0x8E; return 1; } /* jbe/jle */
  if (strcmp(op, ">=") == 0) { *out = is_unsigned ? 0x82 : 0x8C; return 1; } /* jb/jl */
  return 0;
}

/* Ordered float comparison via ucomis. Because ucomis sets CF on "unordered"
 * (NaN), we pick the operand order so the single condition code is NaN-correct
 * (a comparison involving NaN must be false). `swap` requests ucomis(rhs,lhs).
 * For fused branches `cc` is the jcc taken when the comparison is FALSE
 * (branch_zero semantics); otherwise it is the setcc taken when TRUE.
 * Only the ordering operators are handled here; == / != need extra PF handling
 * and are left to the legacy path. */
static int mir_float_cmp_info(const char *op, int fused, int *swap,
                              unsigned char *cc) {
  if (strcmp(op, ">") == 0)  { *swap = 0; *cc = fused ? 0x86 : 0x97; return 1; }
  if (strcmp(op, ">=") == 0) { *swap = 0; *cc = fused ? 0x82 : 0x93; return 1; }
  if (strcmp(op, "<") == 0)  { *swap = 1; *cc = fused ? 0x86 : 0x97; return 1; }
  if (strcmp(op, "<=") == 0) { *swap = 1; *cc = fused ? 0x82 : 0x93; return 1; }
  return 0;
}

/* Float arithmetic operator -> MIR opcode (divide is supported for floats). */
static int mir_float_arith_opcode(const char *op, MirOpcode *out) {
  if (strcmp(op, "+") == 0) { *out = MIR_FADD; return 1; }
  if (strcmp(op, "-") == 0) { *out = MIR_FSUB; return 1; }
  if (strcmp(op, "*") == 0) { *out = MIR_FMUL; return 1; }
  if (strcmp(op, "/") == 0) { *out = MIR_FDIV; return 1; }
  return 0;
}

/* Arithmetic operator -> MIR opcode. Returns 0 if not an arithmetic op we
 * handle (integer divide/modulo are intentionally excluded). */
static int mir_arith_opcode(const char *op, MirOpcode *out) {
  if (strcmp(op, "+") == 0)  { *out = MIR_ADD; return 1; }
  if (strcmp(op, "-") == 0)  { *out = MIR_SUB; return 1; }
  if (strcmp(op, "*") == 0)  { *out = MIR_IMUL; return 1; }
  if (strcmp(op, "&") == 0)  { *out = MIR_AND; return 1; }
  if (strcmp(op, "|") == 0)  { *out = MIR_OR; return 1; }
  if (strcmp(op, "^") == 0)  { *out = MIR_XOR; return 1; }
  if (strcmp(op, "<<") == 0) { *out = MIR_SHL; return 1; }
  if (strcmp(op, ">>") == 0) { *out = MIR_SHR; return 1; } /* SAR chosen by sign */
  return 0;
}

/* Structural equality of two IR operands (for divmod-pair matching). Only the
 * operand kinds that can be a div dividend/divisor are compared; anything else
 * (float/string/none) is treated as unequal. */
static int mir_ir_operand_equal(const IROperand *a, const IROperand *b) {
  if (a->kind != b->kind) {
    return 0;
  }
  switch (a->kind) {
  case IR_OPERAND_TEMP:
  case IR_OPERAND_SYMBOL:
    return a->name && b->name && strcmp(a->name, b->name) == 0;
  case IR_OPERAND_INT:
    return a->int_value == b->int_value;
  default:
    return 0;
  }
}

static int mir_operand_is_unsigned(CodeGenerator *g, BinaryFunctionContext *ctx,
                                   const IROperand *op) {
  Type *t = code_generator_binary_get_operand_type_in_context(g, ctx, op);
  if (!t) {
    return 0; /* default signed */
  }
  return !code_generator_binary_resolved_type_is_signed_integer(t);
}

/* Byte width that constrains an integer comparison operand: its scalar size for
 * a known <=4-byte integer, 8 for a 64-bit integer / pointer / unknown type, and
 * 0 for an INT literal (it constrains nothing — it follows the other operand). */
static int mir_cmp_operand_width(CodeGenerator *g, BinaryFunctionContext *ctx,
                                 const IROperand *op) {
  if (op->kind == IR_OPERAND_INT) {
    return 0;
  }
  Type *t = code_generator_binary_get_operand_type_in_context(g, ctx, op);
  if (!t || code_generator_type_is_aggregate(t) ||
      code_generator_binary_resolved_type_float_bits(t) != 0) {
    return 8;
  }
  int s = code_generator_binary_resolved_type_scalar_size(t);
  return (s == 1 || s == 2 || s == 4) ? s : 8;
}

/* Width at which to compare two integer operands. MIR computes in 64-bit, so a
 * narrow value (e.g. a uint32 product) can carry garbage in its high bits; a
 * full 64-bit compare would then see that garbage and give the wrong answer.
 *
 * C compares at the promoted operand width, and so must MIR. We narrow to a
 * 32-bit cmp when BOTH typed operands are exactly 4-byte (int32/uint32)
 * integers: the 32-bit cmp looks only at the low 32 bits, which are always the
 * true value (the carried garbage lives above bit 31), and the signed/unsigned
 * setcc/jcc the caller picks reads the 32-bit flags — correct for equality AND
 * ordering. 1/2-byte operands and any 8-byte/pointer operand (or missing type
 * info) keep the conservative 64-bit compare. `op` is currently unused but kept
 * so the policy can be refined per operator if ever needed. */
static int mir_int_compare_width(CodeGenerator *g, BinaryFunctionContext *ctx,
                                 const char *op, const IROperand *lhs,
                                 const IROperand *rhs) {
  (void)op;
  int wl = mir_cmp_operand_width(g, ctx, lhs);
  int wr = mir_cmp_operand_width(g, ctx, rhs);
  int m = wl > wr ? wl : wr;
  return m == 4 ? 4 : 8;
}

/* ---- eligibility -------------------------------------------------------- */

static int mir_type_is_gp_scalar(CodeGenerator *g, const char *type_name) {
  Type *t = code_generator_binary_get_resolved_type(g, type_name, 0);
  if (!t) {
    return 0;
  }
  if (code_generator_binary_resolved_type_float_bits(t) != 0) {
    return 0;
  }
  if (code_generator_type_is_aggregate(t)) {
    return 0;
  }
  int sz = code_generator_binary_resolved_type_scalar_size(t);
  return sz == 1 || sz == 2 || sz == 4 || sz == 8;
}

/* A GP scalar OR a float32/float64 (the types MIR can now keep in a register). */
static int mir_type_is_numeric_scalar(CodeGenerator *g, const char *type_name) {
  if (mir_type_is_gp_scalar(g, type_name)) {
    return 1;
  }
  Type *t = code_generator_binary_get_resolved_type(g, type_name, 0);
  return t && code_generator_binary_resolved_type_float_bits(t) != 0;
}

/* A DIRECT small aggregate (struct/array by value, size 1/2/4/8): the Win64 ABI
 * passes and returns it in a single GP register exactly like an integer, so MIR
 * can carry it as an 8-byte value. Its memory home (when its address is taken
 * for field access) is 8 bytes, which covers the whole struct. Larger or
 * non-power-of-2 aggregates are INDIRECT (hidden pointer) and still bail. */
static int mir_type_is_direct_small_aggregate(CodeGenerator *g,
                                              const char *type_name) {
  Type *t = code_generator_binary_get_resolved_type(g, type_name, 0);
  if (!t || !code_generator_type_is_aggregate(t)) {
    return 0;
  }
  if (code_generator_binary_resolved_type_float_bits(t) != 0) {
    return 0;
  }
  if (code_generator_abi_classify(t) != ABI_PASS_DIRECT) {
    return 0;
  }
  size_t sz = code_generator_abi_type_size(t);
  return sz == 1 || sz == 2 || sz == 4 || sz == 8;
}

/* A type MIR can carry as a register-or-home value at a signature/local
 * boundary: a numeric scalar, or a DIRECT small aggregate (treated as 8 bytes).
 */
static int mir_type_is_mir_value(CodeGenerator *g, const char *type_name) {
  return mir_type_is_numeric_scalar(g, type_name) ||
         mir_type_is_direct_small_aggregate(g, type_name);
}

/* An INDIRECT aggregate (struct/array by value, size>8 or non-power-of-2): the
 * Win64 ABI passes it BY REFERENCE — the caller copies it to a temp and passes
 * the address in a GP register. A parameter of this type therefore arrives as a
 * pointer, which MIR can hold as an 8-byte value; the body accesses fields
 * through that pointer (&@p yields the pointer, not a stack home). */
static int mir_type_is_indirect_aggregate(CodeGenerator *g,
                                          const char *type_name) {
  Type *t = code_generator_binary_get_resolved_type(g, type_name, 0);
  return t && code_generator_type_is_aggregate(t) &&
         code_generator_abi_classify(t) == ABI_PASS_INDIRECT;
}

/* A type acceptable as a PARAMETER: a MIR value (scalar / DIRECT small agg) or
 * an INDIRECT aggregate (received by reference as a pointer). */
static int mir_type_is_param_value(CodeGenerator *g, const char *type_name) {
  return mir_type_is_mir_value(g, type_name) ||
         mir_type_is_indirect_aggregate(g, type_name);
}

/* Resolve the type of a NAME that is a parameter or a declared local of this IR
 * function. The symbol table has popped function scope by codegen time, so a
 * direct symbol_table_lookup returns NULL for locals/params; instead read the
 * function signature and DECLARE_LOCAL instructions, exactly as
 * code_generator_binary_get_operand_type_in_context does. *is_param_out (if
 * given) is set when the name is a parameter. Returns NULL for globals/unknown
 * names (which the caller resolves through the global symbol table). */
static Type *mir_local_or_param_type(CodeGenerator *g,
                                     const IRFunction *ir_function,
                                     const char *name, int *is_param_out) {
  if (is_param_out) {
    *is_param_out = 0;
  }
  if (!g || !ir_function || !name) {
    return NULL;
  }
  for (size_t i = 0; i < ir_function->parameter_count; i++) {
    if (ir_function->parameter_names && ir_function->parameter_names[i] &&
        strcmp(ir_function->parameter_names[i], name) == 0) {
      if (is_param_out) {
        *is_param_out = 1;
      }
      return code_generator_binary_get_resolved_type(
          g, ir_function->parameter_types ? ir_function->parameter_types[i]
                                          : NULL,
          0);
    }
  }
  for (size_t i = 0; i < ir_function->instruction_count; i++) {
    const IRInstruction *in = &ir_function->instructions[i];
    if (in->op == IR_OP_DECLARE_LOCAL && in->dest.name &&
        strcmp(in->dest.name, name) == 0 && in->text) {
      return code_generator_binary_get_resolved_type(g, in->text, 0);
    }
  }
  return NULL;
}

/* If `dest` names a signed/unsigned sub-64-bit integer variable (local, param, or
 * global scalar), return its byte width (1/2/4), else 0. Used to keep narrow
 * homes canonical: MIR computes in 64 bits, so an arithmetic result written to
 * a typed int32/uint32/int16/etc. home can carry bits above the type's width.
 * Narrow integer homes wrap to their width, so each such write is followed by
 * sign- or zero-extension of the destination vreg.
 */
static int mir_dest_integer_narrow_width(CodeGenerator *g,
                                         BinaryFunctionContext *ctx,
                                         const IROperand *dest,
                                         int *is_signed_out) {
  if (is_signed_out) {
    *is_signed_out = 0;
  }
  if (!g || !ctx || !ctx->function_name || !dest ||
      dest->kind != IR_OPERAND_SYMBOL || !dest->name) {
    return 0;
  }
  IRFunction *irf =
      code_generator_find_ir_function_binary(g, ctx->function_name);
  if (!irf) {
    return 0;
  }
  Type *t = mir_local_or_param_type(g, irf, dest->name, NULL);
  if (!t && g->symbol_table) {
    /* Not a local/param: a global scalar (its symbol never goes out of
     * scope). The cached-global vreg carries the value across the function
     * body, so it needs the same canonicalization as a local's vreg. */
    Symbol *s = symbol_table_lookup(g->symbol_table, dest->name);
    t = s ? s->type : NULL;
  }
  if (!t || code_generator_type_is_aggregate(t) ||
      code_generator_binary_resolved_type_float_bits(t) != 0) {
    return 0;
  }
  int w = code_generator_binary_resolved_type_scalar_size(t);
  if (is_signed_out) {
    *is_signed_out = code_generator_binary_resolved_type_is_signed_integer(t);
  }
  return (w == 1 || w == 2 || w == 4) ? w : 0;
}

/* True if NAME is an INDIRECT aggregate local or by-reference parameter of this
 * function. MIR only touches such a value through its ADDRESS (field LOAD/STORE
 * off &@sym); a by-NAME use of the whole aggregate (assign, return, call
 * argument) would be miscompiled as an 8-byte MOV, so the eligibility gate
 * forbids it (except `return @local`, handled by Link 2). */
static int mir_name_is_indirect_aggregate(CodeGenerator *g,
                                          const IRFunction *ir_function,
                                          const char *name) {
  Type *t = mir_local_or_param_type(g, ir_function, name, NULL);
  return t && code_generator_type_is_aggregate(t) &&
         code_generator_abi_classify(t) == ABI_PASS_INDIRECT;
}

/* True if NAME is a struct LOCAL (not a by-reference parameter): one that owns a
 * stack home holding the struct itself. `return @local` for an INDIRECT return
 * copies from that home; a by-ref PARAMETER's home holds a pointer, not the
 * struct, so it is excluded (deferred to the fallback). */
static int mir_name_is_indirect_struct_local(CodeGenerator *g,
                                             const IRFunction *ir_function,
                                             const char *name) {
  int is_param = 0;
  Type *t = mir_local_or_param_type(g, ir_function, name, &is_param);
  return t && !is_param && code_generator_type_is_aggregate(t) &&
         code_generator_abi_classify(t) == ABI_PASS_INDIRECT;
}

/* roundup8 byte size of an INDIRECT aggregate type, or 0 if `t` isn't one. */
static int mir_indirect_type_home_bytes(CodeGenerator *g, Type *t) {
  if (!t || !code_generator_type_is_aggregate(t) ||
      code_generator_abi_classify(t) != ABI_PASS_INDIRECT) {
    return 0;
  }
  (void)g;
  return (int)((code_generator_abi_type_size(t) + 7) & ~(size_t)7);
}

/* If TEMP `name` holds an INDIRECT struct VALUE, return its home byte size
 * (roundup8), else 0. The IR routes struct call results and intermediates
 * through temps; a temp's struct size is recovered from its context: the
 * INDIRECT return type of the call that defines it, the INDIRECT param type of
 * a call that consumes it, or the type of a struct SYMBOL it is whole-struct
 * assigned to/from. (Resolution is via calls/symbols only — never transitively
 * through another temp — so it cannot recurse.) */
static int mir_struct_temp_size(CodeGenerator *g, const IRFunction *irf,
                                const char *name) {
  if (!g || !irf || !name || !g->symbol_table) {
    return 0;
  }
  for (size_t i = 0; i < irf->instruction_count; i++) {
    const IRInstruction *in = &irf->instructions[i];
    if (in->op == IR_OP_CALL && in->text) {
      Symbol *cal = symbol_table_lookup(g->symbol_table, in->text);
      if (cal && cal->kind == SYMBOL_FUNCTION) {
        /* defined by a struct-returning call */
        if (in->dest.kind == IR_OPERAND_TEMP && in->dest.name &&
            strcmp(in->dest.name, name) == 0) {
          Type *r = cal->data.function.return_type ? cal->data.function.return_type
                                                   : cal->type;
          int hb = mir_indirect_type_home_bytes(g, r);
          if (hb) {
            return hb;
          }
        }
        /* consumed as a struct-by-value argument */
        if (cal->data.function.parameter_types) {
          for (size_t a = 0; a < in->argument_count &&
                             a < cal->data.function.parameter_count;
               a++) {
            if (in->arguments[a].kind == IR_OPERAND_TEMP &&
                in->arguments[a].name &&
                strcmp(in->arguments[a].name, name) == 0) {
              int hb = mir_indirect_type_home_bytes(
                  g, cal->data.function.parameter_types[a]);
              if (hb) {
                return hb;
              }
            }
          }
        }
      }
    }
    if (in->op == IR_OP_ASSIGN) {
      const IROperand *other = NULL;
      if (in->dest.kind == IR_OPERAND_TEMP && in->dest.name &&
          strcmp(in->dest.name, name) == 0) {
        other = &in->lhs;
      } else if (in->lhs.kind == IR_OPERAND_TEMP && in->lhs.name &&
                 strcmp(in->lhs.name, name) == 0) {
        other = &in->dest;
      }
      if (other && other->kind == IR_OPERAND_SYMBOL && other->name) {
        Type *t = mir_local_or_param_type(g, irf, other->name, NULL);
        int hb = mir_indirect_type_home_bytes(g, t);
        if (hb) {
          return hb;
        }
      }
    }
  }
  return 0;
}

/* Home byte size of an operand that holds an INDIRECT struct VALUE in a stack
 * home we can LEA (a struct LOCAL symbol or a struct TEMP), else 0. A by-ref
 * struct PARAMETER is excluded (its home holds a pointer, not the struct). */
static int mir_operand_struct_home_size(CodeGenerator *g,
                                        const IRFunction *irf,
                                        const IROperand *op) {
  if (op->kind == IR_OPERAND_SYMBOL && op->name) {
    if (!mir_name_is_indirect_struct_local(g, irf, op->name)) {
      return 0;
    }
    Type *t = mir_local_or_param_type(g, irf, op->name, NULL);
    return mir_indirect_type_home_bytes(g, t);
  }
  if (op->kind == IR_OPERAND_TEMP && op->name) {
    return mir_struct_temp_size(g, irf, op->name);
  }
  return 0;
}

/* True if temp `name` holds a float value, judged from the producing
 * instruction's is_float flag (transitively through assign chains and call
 * return types). Uses IR structure only, so it is safe in eligibility (no
 * function context). Conservative: returns 0 when it cannot tell. */
static int mir_temp_is_float(CodeGenerator *g, IRFunction *function,
                             const char *name, int depth) {
  if (!name || depth > 16) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->dest.kind != IR_OPERAND_TEMP || !in->dest.name ||
        strcmp(in->dest.name, name) != 0) {
      continue;
    }
    if (in->is_float) {
      /* A comparison's is_float flag describes its OPERANDS; the RESULT is an
       * integer 0/1 (ucomis + setcc / fused jcc), so it is a GP value and is
       * fine as a branch condition. */
      if (in->op == IR_OP_BINARY && in->text && mir_is_comparison(in->text)) {
        return 0;
      }
      return 1;
    }
    if (in->op == IR_OP_ASSIGN && in->lhs.kind == IR_OPERAND_TEMP) {
      return mir_temp_is_float(g, function, in->lhs.name, depth + 1);
    }
    if (in->op == IR_OP_CALL && in->text && g->symbol_table) {
      Symbol *callee = symbol_table_lookup(g->symbol_table, in->text);
      if (callee && callee->kind == SYMBOL_FUNCTION) {
        return code_generator_binary_resolved_type_float_bits(
                   callee->data.function.return_type) != 0;
      }
    }
    if (in->op == IR_OP_CALL_INDIRECT && g->symbol_table &&
        in->lhs.kind == IR_OPERAND_SYMBOL && in->lhs.name) {
      Type *ft = mir_local_or_param_type(g, function, in->lhs.name, NULL);
      Symbol *callee = ft ? NULL : symbol_table_lookup(g->symbol_table,
                                                       in->lhs.name);
      if (ft && ft->kind != TYPE_FUNCTION_POINTER) {
        ft = NULL;
      }
      if (!ft) {
        ft = (callee && callee->type &&
              callee->type->kind == TYPE_FUNCTION_POINTER)
                 ? callee->type
                 : NULL;
      }
      return code_generator_binary_resolved_type_float_bits(
                 ft ? ft->fn_return_type : NULL) != 0;
    }
    return 0;
  }
  return 0;
}

/* A direct call MIR can lower: a known function, <=4 register arguments all of
 * GP-scalar type (float args are deferred), a non-INDIRECT (register) return,
 * and simple argument/destination operands. */
static void mir_call_trace(const char *sub) {
  if (mir_env_trace()) {
    fprintf(stderr, "MIR-CALLBAIL\t%s\n", sub);
  }
}

/* The runtime abort traps the compiler injects for failed safety checks
 * (bounds, overflow, null, ...). They never return (puts+exit / handler abort),
 * so MIR can lower them as a self-contained terminal sequence. */
static int mir_call_is_runtime_trap(const IRInstruction *in) {
  return in->text && (strcmp(in->text, "mettle_crash_trap_ex") == 0 ||
                      strcmp(in->text, "mettle_crash_trap") == 0);
}

static int mir_arg_float_bits(CodeGenerator *g, const IRFunction *ir_function,
                              const IROperand *op);

static Type *mir_indirect_call_type(CodeGenerator *g,
                                    const IRFunction *ir_function,
                                    const IRInstruction *in) {
  if (!g || !in || in->lhs.kind != IR_OPERAND_SYMBOL || !in->lhs.name) {
    return NULL;
  }
  Type *local = mir_local_or_param_type(g, ir_function, in->lhs.name, NULL);
  if (local && local->kind == TYPE_FUNCTION_POINTER) {
    return local;
  }
  Symbol *sym = g->symbol_table ? symbol_table_lookup(g->symbol_table,
                                                      in->lhs.name)
                                : NULL;
  return (sym && sym->type && sym->type->kind == TYPE_FUNCTION_POINTER)
             ? sym->type
             : NULL;
}

static IRFunction *mir_find_ir_function_named(CodeGenerator *g,
                                              const char *name) {
  if (!g || !name || !name[0]) {
    return NULL;
  }
  IRFunction *f = code_generator_find_ir_function_binary(g, name);
  if (!f && name[0] == '@') {
    f = code_generator_find_ir_function_binary(g, name + 1);
  }
  return f;
}

static int mir_call_indirect_is_supported(CodeGenerator *g,
                                          const IRFunction *ir_function,
                                          const IRInstruction *in) {
  if (!in || in->lhs.kind != IR_OPERAND_SYMBOL || !in->lhs.name) {
    mir_call_trace("indirect_no_symbol");
    return 0;
  }
  if (in->argument_count > MIR_MAX_PARAMS) {
    mir_call_trace("indirect_args>max");
    return 0;
  }
  Type *ft = mir_indirect_call_type(g, ir_function, in);
  if (!ft) {
    mir_call_trace("indirect_no_type");
    return 0;
  }
  if (in->argument_count != ft->fn_param_count) {
    mir_call_trace("indirect_arity_mismatch");
    return 0;
  }
  Type *ret = ft->fn_return_type;
  if (!code_generator_binary_resolved_type_is_abi_supported(ret, 1)) {
    mir_call_trace("indirect_ret_unsupported");
    return 0;
  }
  if (ret && (code_generator_type_is_aggregate(ret) ||
              code_generator_binary_type_is_string(ret))) {
    mir_call_trace("indirect_ret_aggregate");
    return 0;
  }
  if (in->dest.kind != IR_OPERAND_NONE && in->dest.kind != IR_OPERAND_TEMP &&
      in->dest.kind != IR_OPERAND_SYMBOL) {
    mir_call_trace("indirect_dest_kind");
    return 0;
  }

  const BinaryAbi *call_abi = code_generator_binary_active_abi();
  for (size_t a = 0; a < in->argument_count; a++) {
    Type *pt = ft->fn_param_types ? ft->fn_param_types[a] : NULL;
    const IROperand *arg = &in->arguments[a];
    if (!pt) {
      mir_call_trace("indirect_arg_no_type");
      return 0;
    }
    if (!code_generator_binary_resolved_type_is_abi_supported(pt, 0)) {
      mir_call_trace("indirect_arg_unsupported");
      return 0;
    }
    if (code_generator_type_is_aggregate(pt) ||
        code_generator_binary_type_is_string(pt) ||
        code_generator_abi_classify(pt) == ABI_PASS_INDIRECT) {
      mir_call_trace("indirect_arg_aggregate");
      return 0;
    }
    if (code_generator_binary_resolved_type_float_bits(pt) != 0) {
      if (mir_arg_float_bits(g, ir_function, arg) == 0) {
        mir_call_trace("indirect_arg_float_nonfloat_src");
        return 0;
      }
      if (call_abi->counts_classes_separately) {
        size_t fbefore = 0;
        for (size_t b = 0; b < a; b++) {
          Type *bt = ft->fn_param_types ? ft->fn_param_types[b] : NULL;
          if (bt && code_generator_binary_resolved_type_float_bits(bt) != 0) {
            fbefore++;
          }
        }
        if (fbefore >= (size_t)call_abi->float_param_count) {
          mir_call_trace("indirect_arg_float_stack");
          return 0;
        }
      } else if (a >= (size_t)call_abi->int_param_count) {
        mir_call_trace("indirect_arg_float_stack");
        return 0;
      }
      continue;
    }
    if (arg->kind != IR_OPERAND_TEMP && arg->kind != IR_OPERAND_SYMBOL &&
        arg->kind != IR_OPERAND_INT && arg->kind != IR_OPERAND_STRING) {
      mir_call_trace("indirect_arg_operand_kind");
      return 0;
    }
    if (arg->kind == IR_OPERAND_STRING &&
        !code_generator_binary_type_is_cstring(pt)) {
      mir_call_trace("indirect_arg_string_non_cstring");
      return 0;
    }
  }
  return 1;
}

/* Classify an IR_OP_ADDRESS_OF target. */
typedef enum {
  MIR_ADDROF_UNSUPPORTED = 0, /* function/string/other: deferred */
  MIR_ADDROF_LOCAL,           /* scalar/DIRECT-agg local or parameter (lea home) */
  MIR_ADDROF_GLOBAL,          /* scalar global (lea RIP-relative) */
  MIR_ADDROF_FUNCTION,        /* function symbol (lea code address) */
  MIR_ADDROF_INDIRECT_PARAM   /* INDIRECT-aggregate param: &@p IS the by-ref
                                 pointer, so copy the param value (no home) */
} MirAddrofKind;

static MirAddrofKind mir_addressof_kind(CodeGenerator *g,
                                        const IRFunction *ir_function,
                                        const IRInstruction *in) {
  if (in->lhs.kind != IR_OPERAND_SYMBOL || !in->lhs.name) {
    return MIR_ADDROF_UNSUPPORTED;
  }
  Symbol *sym = g && g->symbol_table
                    ? symbol_table_lookup(g->symbol_table, in->lhs.name)
                    : NULL;
  if ((sym && sym->kind == SYMBOL_FUNCTION) ||
      mir_find_ir_function_named(g, in->lhs.name)) {
    return MIR_ADDROF_FUNCTION;
  }
  /* Resolve the target as a local/parameter of this function from the IR (the
   * symbol table has popped function scope by codegen time, so a direct lookup
   * fails for locals/params). A NULL type means the name is a global/external. */
  int is_param = 0;
  Type *t = mir_local_or_param_type(g, ir_function, in->lhs.name, &is_param);
  if (!t) {
    /* Not a local/param: a global (or extern). Only a plain scalar global is
     * supported (cached, kept coherent via flush/reload around pointer ops). */
    return mir_name_is_global_scalar(g, in->lhs.name) ? MIR_ADDROF_GLOBAL
                                                      : MIR_ADDROF_UNSUPPORTED;
  }
  if (t->kind == TYPE_STRING) {
    return MIR_ADDROF_UNSUPPORTED; /* string has its own (fat-pointer) address form */
  }
  /* An INDIRECT-aggregate parameter is passed by reference: the parameter value
   * already IS the struct's address, so &@p just yields that pointer. */
  if (is_param && code_generator_type_is_aggregate(t) &&
      code_generator_abi_classify(t) == ABI_PASS_INDIRECT) {
    return MIR_ADDROF_INDIRECT_PARAM;
  }
  return MIR_ADDROF_LOCAL; /* scalar/DIRECT/INDIRECT-agg local or param: lea home */
}

/* Float bit-width (32/64) of a call-argument value operand for the eligibility
 * gate, which (unlike lowering) has no BinaryFunctionContext. Uses the operand's
 * own float_bits tag and the symbol table only — the same signals lowering's
 * code_generator_binary_operand_float_bits treats as authoritative (it returns
 * the operand's float_bits first), so the gate and lowering agree on which args
 * are float. Returns 0 for a non-float or undeterminable operand (gate defers). */
static int mir_arg_float_bits(CodeGenerator *g, const IRFunction *ir_function,
                              const IROperand *op) {
  if (!op) {
    return 0;
  }
  if (op->kind == IR_OPERAND_FLOAT) {
    return op->float_bits == 32 ? 32 : 64;
  }
  if (op->kind == IR_OPERAND_TEMP || op->kind == IR_OPERAND_SYMBOL) {
    if (op->float_bits == 32 || op->float_bits == 64) {
      return op->float_bits;
    }
    if (op->kind == IR_OPERAND_SYMBOL && op->name) {
      /* A float LOCAL or PARAMETER: resolve its declared type from the IR (the
       * symbol table holds only globals at codegen time, scope having been
       * popped). This is exactly the type lowering's mir_value_operand will see,
       * so the gate and the homing agree on which args are float. */
      Type *lt = mir_local_or_param_type(g, ir_function, op->name, NULL);
      if (lt) {
        return code_generator_binary_resolved_type_float_bits(lt);
      }
      if (g && g->symbol_table) {
        Symbol *s = symbol_table_lookup(g->symbol_table, op->name);
        if (s) {
          return code_generator_binary_resolved_type_float_bits(s->type);
        }
      }
    }
  }
  return 0;
}

static int mir_call_is_supported(CodeGenerator *g,
                                 const IRFunction *ir_function,
                                 const IRInstruction *in) {
  if (!in->text || in->text[0] == '\0') {
    mir_call_trace("no_name");
    return 0;
  }
  /* Runtime safety-check traps are terminal and lowered specially (MIR_TRAP),
   * so they bypass the normal known-function / argument-shape requirements. */
  if (mir_call_is_runtime_trap(in)) {
    return 1;
  }
  if (in->argument_count > MIR_MAX_PARAMS) {
    mir_call_trace("args>max");
    return 0;
  }
  Symbol *callee =
      g->symbol_table ? symbol_table_lookup(g->symbol_table, in->text) : NULL;
  if (!callee || callee->kind != SYMBOL_FUNCTION) {
    mir_call_trace("not_known_function");
    return 0;
  }
  Type *ret = callee->data.function.return_type
                  ? callee->data.function.return_type
                  : callee->type;
  int hidden = 0;
  if (ret && code_generator_abi_classify(ret) == ABI_PASS_INDIRECT) {
    /* struct-by-value return: the caller passes a hidden out-pointer as the
     * first integer arg, pointed at the destination struct's home (a struct
     * LOCAL or a struct TEMP), so the callee writes the result directly there. */
    if (mir_operand_struct_home_size(g, ir_function, &in->dest) == 0) {
      mir_call_trace("ret_indirect");
      return 0;
    }
    hidden = 1; /* hidden out-pointer occupies the first positional ABI slot */
  }
  if (callee->data.function.parameter_count != in->argument_count) {
    mir_call_trace("arity_mismatch");
    return 0; /* variadic / arity mismatch: not yet */
  }
  /* The positional ABI slot count that lands in a register (Win64: 4 shared
   * int/float; SysV draws int and float from separate larger pools). Float args
   * are homed only when they fall in an XMM register; a float STACK arg (5th+
   * positional under Win64) is still deferred to the fallback. */
  const BinaryAbi *call_abi = code_generator_binary_active_abi();
  for (size_t a = 0; a < in->argument_count; a++) {
    Type *pt = callee->data.function.parameter_types
                   ? callee->data.function.parameter_types[a]
                   : NULL;
    const IROperand *arg = &in->arguments[a];
    if (!pt) {
      mir_call_trace("arg_no_type");
      return 0;
    }
    if (code_generator_binary_resolved_type_float_bits(pt) != 0) {
      /* Float parameter: homeable when (1) the argument is itself a float value
       * (a float temp/local/param or a float literal) — an int->float implicit
       * conversion at the call site is left to the fallback — and (2) it lands in
       * an XMM register, not a stack slot. */
      if (mir_arg_float_bits(g, ir_function, arg) == 0) {
        mir_call_trace("arg_float_nonfloat_src");
        return 0;
      }
      size_t slot = a + (size_t)hidden;
      if (call_abi->counts_classes_separately) {
        /* SysV: floats draw from their own register file in argument order. */
        size_t fbefore = 0;
        for (size_t b = 0; b < a; b++) {
          Type *bt = callee->data.function.parameter_types
                         ? callee->data.function.parameter_types[b]
                         : NULL;
          if (bt && code_generator_binary_resolved_type_float_bits(bt) != 0) {
            fbefore++;
          }
        }
        if (fbefore >= (size_t)call_abi->float_param_count) {
          mir_call_trace("arg_float_stack");
          return 0;
        }
      } else if (slot >= (size_t)call_abi->int_param_count) {
        mir_call_trace("arg_float_stack");
        return 0;
      }
      continue;
    }
    if (code_generator_abi_classify(pt) == ABI_PASS_INDIRECT) {
      /* struct passed BY VALUE: the caller copies it to an outgoing temp and
       * passes the temp's address. The source must hold the struct in a LEA-able
       * home — a struct LOCAL or a struct TEMP; a by-ref param source is
       * deferred (its home holds a pointer, not the struct). */
      if (mir_operand_struct_home_size(g, ir_function, arg) == 0) {
        mir_call_trace("arg_struct_nonlocal");
        return 0;
      }
      continue;
    }
    if (arg->kind != IR_OPERAND_TEMP && arg->kind != IR_OPERAND_SYMBOL &&
        arg->kind != IR_OPERAND_INT && arg->kind != IR_OPERAND_STRING) {
      mir_call_trace("arg_operand_kind");
      return 0;
    }
    if (arg->kind == IR_OPERAND_STRING &&
        !code_generator_binary_type_is_cstring(pt)) {
      /* A string literal is only lowered to a bare cstring (char* in one GP
       * register) when the parameter is itself a cstring — matching the fallback
       * emitter (emit_call_argument_load). A `string` fat-pointer parameter
       * ({chars,length}) needs the struct ABI, which MIR does not build yet. */
      mir_call_trace("arg_string_non_cstring");
      return 0;
    }
  }
  if (in->dest.kind != IR_OPERAND_NONE && in->dest.kind != IR_OPERAND_TEMP &&
      in->dest.kind != IR_OPERAND_SYMBOL) {
    mir_call_trace("dest_kind");
    return 0;
  }
  return 1;
}

/* Pure-ish scan: returns 1 if every instruction is in the supported set and the
 * signature is GP-only. Uses generator for type queries; no MIR built yet. */
int mir_function_is_eligible(CodeGenerator *generator,
                             FunctionDeclaration *function_data,
                             IRFunction *ir_function) {
  if (!generator || !function_data || !ir_function) {
    return 0;
  }
  g_mir_gate_fn_size = 0;
  if (ir_explain_enabled()) {
    for (size_t i = 0; i < ir_function->instruction_count; i++) {
      if (ir_function->instructions[i].op != IR_OP_NOP) {
        g_mir_gate_fn_size++;
      }
    }
  }
  /* Kill switch for bisecting MIR vs legacy regressions. */
  {
    const char *off = mir_env_mir();
    if (off && off[0] == '0') {
      return 0;
    }
    /* Bisect: comma-separated list of function names forced to fallback. */
    const char *skip = mir_env_skipfn();
    if (skip && function_data->name) {
      const char *nm = function_data->name;
      size_t nl = strlen(nm);
      const char *p = skip;
      while (*p) {
        const char *c = strchr(p, ',');
        size_t seg = c ? (size_t)(c - p) : strlen(p);
        if (seg == nl && strncmp(p, nm, nl) == 0) {
          return 0;
        }
        if (!c) break;
        p = c + 1;
      }
    }
  }
  /* Stage 2 targets plain --release codegen only: no debug line markers,
   * stack-trace ranges, or profiling instrumentation. */
  if (generator->generate_debug_info || generator->generate_stack_trace_support ||
      generator->profile_runtime) {
    return 0;
  }
  /* Signature: <=4 GP params, GP-or-void return, no indirect return. */
  if (function_data->parameter_count > MIR_MAX_PARAMS) {
    return mir_trace_bail(function_data, "sig:params>max");
  }
  {
    int pis_float[MIR_MAX_PARAMS];
    for (size_t i = 0; i < function_data->parameter_count; i++) {
      const char *pt = function_data->parameter_types
                           ? function_data->parameter_types[i]
                           : NULL;
      if (!mir_type_is_param_value(generator, pt)) {
        return mir_trace_bail(function_data, "sig:param_nonscalar");
      }
      Type *rt = code_generator_binary_get_resolved_type(generator, pt, 0);
      pis_float[i] =
          (rt && code_generator_binary_resolved_type_float_bits(rt) != 0) ? 1 : 0;
    }
    /* GP params beyond the ABI's argument registers are homed from the caller's
     * stack frame (handled below). A FLOAT param landing on the stack is not
     * homed yet, so defer those functions to the fallback. An INDIRECT struct
     * return consumes the first integer argument slot as a hidden out-pointer,
     * shifting every user parameter up by one — model that here so the on-stack
     * detection matches the prologue's homing exactly. */
    int hidden = mir_type_is_indirect_aggregate(generator,
                                                function_data->return_type)
                     ? 1
                     : 0;
    if (function_data->parameter_count > 0) {
      const BinaryAbi *abi = code_generator_binary_active_abi();
      int aug_float[MIR_MAX_PARAMS + 1];
      BinaryArgLocation locs[MIR_MAX_PARAMS + 1];
      size_t n = function_data->parameter_count + (size_t)hidden;
      if (n > MIR_MAX_PARAMS) {
        return mir_trace_bail(function_data, "sig:params>max");
      }
      if (hidden) {
        aug_float[0] = 0; /* hidden out-pointer is an integer arg */
      }
      for (size_t i = 0; i < function_data->parameter_count; i++) {
        aug_float[i + (size_t)hidden] = pis_float[i];
      }
      if (!code_generator_binary_compute_arg_layout(abi, aug_float, n, locs,
                                                    NULL)) {
        return mir_trace_bail(function_data, "sig:arg_layout");
      }
      for (size_t i = 0; i < function_data->parameter_count; i++) {
        if (pis_float[i] &&
            locs[i + (size_t)hidden].kind == BINARY_ARG_ON_STACK) {
          return mir_trace_bail(function_data, "sig:float_stack_param");
        }
      }
    }
  }
  /* A non-void return must be a register value (scalar / DIRECT small agg) OR an
   * INDIRECT aggregate returned via the hidden out-pointer (handled at RETURN). */
  if (function_data->return_type && function_data->return_type[0] &&
      strcmp(function_data->return_type, "void") != 0 &&
      !mir_type_is_mir_value(generator, function_data->return_type) &&
      !mir_type_is_indirect_aggregate(generator, function_data->return_type)) {
    return mir_trace_bail(function_data, "sig:return_nonscalar");
  }

  /* Collect the names that are defined inside the function: parameters and
   * declared locals. Any SYMBOL operand naming something outside this set is a
   * global (or otherwise externally-defined) value. Those become vregs that no
   * prologue/def ever initializes, so the function is not yet MIR-eligible. */
  MirNameMap defined = {0};
  MirFunction scratch_fn;
  memset(&scratch_fn, 0, sizeof(scratch_fn));
  int globals_ok = 1;
  for (size_t i = 0; i < function_data->parameter_count; i++) {
    if (function_data->parameter_names[i]) {
      mir_name_map_get_or_add(&defined, &scratch_fn,
                              function_data->parameter_names[i], 0, MIR_RC_GP,
                              8);
    }
  }
  for (size_t i = 0; i < ir_function->instruction_count; i++) {
    const IRInstruction *in = &ir_function->instructions[i];
    if (in->op == IR_OP_DECLARE_LOCAL && in->dest.kind == IR_OPERAND_SYMBOL &&
        in->dest.name) {
      mir_name_map_get_or_add(&defined, &scratch_fn, in->dest.name, 0,
                              MIR_RC_GP, 8);
    }
  }
  if (scratch_fn.has_error) {
    mir_name_map_destroy(&defined);
    mir_function_destroy(&scratch_fn);
    return 0;
  }
  /* Any SYMBOL operand not defined in this function is a global access. It is
   * eligible iff it is a plain scalar global (no address-of in scope — an
   * IR_OP_ADDRESS_OF would be rejected below, so no aliasing pointer can reach
   * it). Calls are fine: the lowering flushes written globals before each call
   * and reloads cached globals after, keeping memory authoritative across the
   * call boundary. */
  for (size_t i = 0; i < ir_function->instruction_count && globals_ok; i++) {
    const IRInstruction *in = &ir_function->instructions[i];
    /* An undefined SYMBOL written here is a global STORE. */
    if (in->dest.kind == IR_OPERAND_SYMBOL && in->dest.name) {
      int found = 0;
      for (size_t j = 0; j < defined.count; j++) {
        if (strcmp(defined.items[j].name, in->dest.name) == 0) {
          found = 1;
          break;
        }
      }
      if (!found && !mir_name_is_global_scalar(generator, in->dest.name)) {
        globals_ok = 0;
        break;
      }
    }
    /* An undefined SYMBOL read must be a global scalar. */
    const IROperand *reads[2] = {&in->lhs, &in->rhs};
    for (int k = 0; k < 2; k++) {
      if (in->op == IR_OP_ADDRESS_OF && reads[k] == &in->lhs) {
        continue; /* &symbol names an address target, not a by-value read. */
      }
      if (reads[k]->kind == IR_OPERAND_SYMBOL && reads[k]->name) {
        int found = 0;
        for (size_t j = 0; j < defined.count; j++) {
          if (strcmp(defined.items[j].name, reads[k]->name) == 0) {
            found = 1;
            break;
          }
        }
        if (!found && !mir_name_is_global_scalar(generator, reads[k]->name)) {
          globals_ok = 0;
          break;
        }
      }
    }
    /* Undefined SYMBOL call arguments are global reads too (e.g. f(g)). They
     * must be scalar globals so the entry-load pass can cache them; otherwise
     * the value path would map them to an undefined vreg. */
    for (size_t a = 0; a < in->argument_count && globals_ok; a++) {
      const IROperand *arg = &in->arguments[a];
      if (arg->kind != IR_OPERAND_SYMBOL || !arg->name) {
        continue;
      }
      int found = 0;
      for (size_t j = 0; j < defined.count; j++) {
        if (strcmp(defined.items[j].name, arg->name) == 0) {
          found = 1;
          break;
        }
      }
      if (!found && !mir_name_is_global_scalar(generator, arg->name)) {
        globals_ok = 0;
        break;
      }
    }
  }
  mir_name_map_destroy(&defined);
  mir_function_destroy(&scratch_fn);
  if (!globals_ok) {
    return mir_trace_bail(function_data, "global_access");
  }

  for (size_t i = 0; i < ir_function->instruction_count; i++) {
    const IRInstruction *in = &ir_function->instructions[i];
    /* Whole-struct by-name guard: an INDIRECT aggregate (struct local or
     * by-reference param) may only be DECLARED or have its ADDRESS taken; MIR
     * reaches its fields exclusively through &@sym + offset memory ops. Any
     * other by-name appearance (assign/return/call-arg/store value) would copy
     * just the low 8 bytes, so defer such a function to the fallback. */
    {
      const IROperand *whole[3] = {&in->dest, &in->lhs, &in->rhs};
      for (int k = 0; k < 3; k++) {
        const IROperand *o = whole[k];
        if (o->kind != IR_OPERAND_SYMBOL || !o->name ||
            !mir_name_is_indirect_aggregate(generator, ir_function, o->name)) {
          continue;
        }
        int allowed =
            (in->op == IR_OP_DECLARE_LOCAL && o == &in->dest) ||
            (in->op == IR_OP_ADDRESS_OF && o == &in->lhs) ||
            (in->op == IR_OP_RETURN && o == &in->lhs &&
             mir_name_is_indirect_struct_local(generator, ir_function, o->name)) ||
            /* `@local = f()` for a struct-returning callee: the call writes the
             * struct directly into the dest local's home via the hidden return
             * pointer (mir_call_is_supported validates the callee returns
             * INDIRECT). */
            (in->op == IR_OP_CALL && o == &in->dest &&
             mir_name_is_indirect_struct_local(generator, ir_function, o->name)) ||
            /* Whole-struct ASSIGN `@a <- @b` / `@a <- %t` / `%t <- @a`: a struct
             * COPY between two LEA-able struct homes (lowered via rep-movsb), so
             * both operands may name a struct symbol. */
            (in->op == IR_OP_ASSIGN && (o == &in->dest || o == &in->lhs) &&
             mir_operand_struct_home_size(generator, ir_function, &in->dest) >
                 0 &&
             mir_operand_struct_home_size(generator, ir_function, &in->lhs) > 0);
        if (!allowed) {
          return mir_trace_bail(function_data, "indirect_agg_byname");
        }
      }
      for (size_t a = 0; a < in->argument_count; a++) {
        if (in->arguments[a].kind == IR_OPERAND_SYMBOL &&
            in->arguments[a].name &&
            mir_name_is_indirect_aggregate(generator, ir_function,
                                           in->arguments[a].name) &&
            /* A struct LOCAL passed by value is allowed (Link 4 copies it to an
             * outgoing temp; mir_call_is_supported validates the callee param).
             * A by-ref param source is still rejected. */
            !(in->op == IR_OP_CALL &&
              mir_name_is_indirect_struct_local(generator, ir_function,
                                                in->arguments[a].name))) {
          return mir_trace_bail(function_data, "indirect_agg_byname");
        }
      }
    }
    switch (in->op) {
    case IR_OP_NOP:
    case IR_OP_LABEL:
    case IR_OP_JUMP:
      break;
    case IR_OP_DECLARE_LOCAL:
      /* A DIRECT small-aggregate local is allowed: field access lowers to
       * &local + offset + LOAD/STORE (all supported), and when its address is
       * taken it becomes memory-resident with an 8-byte home covering it. An
       * INDIRECT struct local is also allowed: it gets a multi-slot home sized
       * to the whole struct (home_bytes), and the same &local + offset + memory
       * machinery reaches every field. Whole-struct by-name uses of it are
       * rejected by the guard below, so only field access ever touches it. */
      if (in->text && !mir_type_is_mir_value(generator, in->text) &&
          !mir_type_is_indirect_aggregate(generator, in->text)) {
        return mir_trace_bail(function_data, "declare_local:nonscalar");
      }
      break;
    case IR_OP_BRANCH_ZERO:
      if (in->lhs.kind != IR_OPERAND_TEMP && in->lhs.kind != IR_OPERAND_SYMBOL) {
        return 0;
      }
      /* branch_zero on a float value (e.g. errdefer on a float return) needs a
       * float-zero compare; float branches are deferred -> fall back. */
      if (in->lhs.kind == IR_OPERAND_TEMP &&
          mir_temp_is_float(generator, ir_function, in->lhs.name, 0)) {
        return 0;
      }
      break;
    case IR_OP_BRANCH_EQ: {
      /* if (lhs == rhs) goto label: integer equality (switch/match dispatch).
       * Both operands must be register-resident or an int literal; float
       * equality would need ucomis, so defer it. */
      const IROperand *eq[2] = {&in->lhs, &in->rhs};
      for (int k = 0; k < 2; k++) {
        if (eq[k]->kind != IR_OPERAND_TEMP && eq[k]->kind != IR_OPERAND_SYMBOL &&
            eq[k]->kind != IR_OPERAND_INT) {
          return mir_trace_bail(function_data, "branch_eq:operand_kind");
        }
        if (eq[k]->kind == IR_OPERAND_TEMP &&
            mir_temp_is_float(generator, ir_function, eq[k]->name, 0)) {
          return mir_trace_bail(function_data, "branch_eq:float");
        }
      }
      if (in->is_float) {
        return mir_trace_bail(function_data, "branch_eq:float");
      }
      break;
    }
    case IR_OP_ASSIGN:
      if (in->dest.kind != IR_OPERAND_TEMP && in->dest.kind != IR_OPERAND_SYMBOL) {
        return 0;
      }
      if (in->lhs.kind != IR_OPERAND_TEMP && in->lhs.kind != IR_OPERAND_SYMBOL &&
          in->lhs.kind != IR_OPERAND_INT && in->lhs.kind != IR_OPERAND_FLOAT) {
        return 0;
      }
      break;
    case IR_OP_BINARY: {
      MirOpcode tmp;
      if (!in->text) {
        return 0;
      }
      if (in->is_float) {
        /* Float arithmetic, or an ordered float comparison (<,<=,>,>=). */
        int sw;
        unsigned char fcc;
        if (!mir_float_arith_opcode(in->text, &tmp) &&
            !mir_float_cmp_info(in->text, 0, &sw, &fcc)) {
          return 0;
        }
      } else if (!mir_arith_opcode(in->text, &tmp) &&
                 !mir_is_comparison(in->text) &&
                 strcmp(in->text, "/") != 0 && strcmp(in->text, "%") != 0) {
        return mir_trace_bail(function_data, "binary:other");
      }
      if (in->dest.kind != IR_OPERAND_TEMP && in->dest.kind != IR_OPERAND_SYMBOL) {
        return 0;
      }
      for (int k = 0; k < 2; k++) {
        const IROperand *o = k == 0 ? &in->lhs : &in->rhs;
        if (o->kind != IR_OPERAND_TEMP && o->kind != IR_OPERAND_SYMBOL &&
            o->kind != IR_OPERAND_INT && o->kind != IR_OPERAND_FLOAT) {
          return 0;
        }
      }
      break;
    }
    case IR_OP_CAST:
      /* Any scalar numeric cast: int<->int, int<->float, float<->float. The
       * direction is resolved from operand types during lowering, which is
       * exhaustive for these, so it cannot fail mid-function. */
      if (in->dest.kind != IR_OPERAND_TEMP && in->dest.kind != IR_OPERAND_SYMBOL) {
        return 0;
      }
      if (in->lhs.kind != IR_OPERAND_TEMP && in->lhs.kind != IR_OPERAND_SYMBOL &&
          in->lhs.kind != IR_OPERAND_INT && in->lhs.kind != IR_OPERAND_FLOAT) {
        return 0;
      }
      break;
    case IR_OP_UNARY:
      /* Integer unary `-`, `~`, `+`, `!`; float unary `-` (negate as 0-x) and
       * `+` (copy). Float `~`/`!` are not valid and popcnt is deferred. */
      if (!in->text) {
        return mir_trace_bail(function_data, "unary:float_or_unsupported");
      }
      if (in->is_float) {
        if (strcmp(in->text, "-") != 0 && strcmp(in->text, "+") != 0) {
          return mir_trace_bail(function_data, "unary:float_or_unsupported");
        }
      } else if (strcmp(in->text, "-") != 0 && strcmp(in->text, "~") != 0 &&
                 strcmp(in->text, "+") != 0 && strcmp(in->text, "!") != 0) {
        return mir_trace_bail(function_data, "unary:float_or_unsupported");
      }
      if (in->dest.kind != IR_OPERAND_TEMP && in->dest.kind != IR_OPERAND_SYMBOL) {
        return 0;
      }
      if (in->lhs.kind != IR_OPERAND_TEMP && in->lhs.kind != IR_OPERAND_SYMBOL &&
          in->lhs.kind != IR_OPERAND_INT && in->lhs.kind != IR_OPERAND_FLOAT) {
        return 0;
      }
      break;
    case IR_OP_LOAD:
      /* `%t <- *"literal" [8]` reads the data-pointer field of a string
       * literal's fat struct: the value IS the address of a NUL-terminated
       * .rdata cstring, so it lowers to MIR_LEA_CSTR (the same materialization
       * used for string-literal call arguments). Any other width/shape on a
       * STRING operand is deferred. */
      if (in->lhs.kind == IR_OPERAND_STRING) {
        if (in->is_float || in->rhs.kind != IR_OPERAND_INT ||
            in->rhs.int_value != 8) {
          return mir_trace_bail(function_data, "load:string_shape");
        }
        if (in->dest.kind != IR_OPERAND_TEMP &&
            in->dest.kind != IR_OPERAND_SYMBOL) {
          return mir_trace_bail(function_data, "load:dest");
        }
        break;
      }
      if (in->lhs.kind != IR_OPERAND_TEMP && in->lhs.kind != IR_OPERAND_SYMBOL) {
        return mir_trace_bail(function_data, "load:address_kind");
      }
      if (in->dest.kind != IR_OPERAND_TEMP && in->dest.kind != IR_OPERAND_SYMBOL) {
        return mir_trace_bail(function_data, "load:dest");
      }
      break;
    case IR_OP_STORE:
      if (in->dest.kind != IR_OPERAND_TEMP && in->dest.kind != IR_OPERAND_SYMBOL) {
        return 0; /* address */
      }
      if (in->lhs.kind != IR_OPERAND_TEMP && in->lhs.kind != IR_OPERAND_SYMBOL &&
          in->lhs.kind != IR_OPERAND_INT && in->lhs.kind != IR_OPERAND_FLOAT) {
        return 0; /* value */
      }
      break;
    case IR_OP_PREFETCH:
      if (in->lhs.kind != IR_OPERAND_TEMP && in->lhs.kind != IR_OPERAND_SYMBOL) {
        return mir_trace_bail(function_data, "prefetch:addr");
      }
      break;
    case IR_OP_SELECT: {
      /* dest = (cond != 0) ? then : else. Each of cond/then/else may be a
       * temp/symbol/int; the dest is a temp/symbol. */
      const IROperand *sops[3] = {&in->lhs, &in->rhs,
                                  in->argument_count > 0 ? &in->arguments[0]
                                                         : NULL};
      if (!sops[2]) {
        return mir_trace_bail(function_data, "select:no_else");
      }
      for (int s = 0; s < 3; s++) {
        if (sops[s]->kind != IR_OPERAND_TEMP &&
            sops[s]->kind != IR_OPERAND_SYMBOL &&
            sops[s]->kind != IR_OPERAND_INT) {
          return mir_trace_bail(function_data, "select:operand_kind");
        }
      }
      if (in->dest.kind != IR_OPERAND_TEMP && in->dest.kind != IR_OPERAND_SYMBOL) {
        return mir_trace_bail(function_data, "select:dest_kind");
      }
      break;
    }
    case IR_OP_RETURN:
      if (in->lhs.kind != IR_OPERAND_NONE && in->lhs.kind != IR_OPERAND_TEMP &&
          in->lhs.kind != IR_OPERAND_SYMBOL && in->lhs.kind != IR_OPERAND_INT) {
        return 0;
      }
      /* An INDIRECT-returning function only handles `return @struct_local`: the
       * RETURN lowering copies from the local's home into the hidden slot. Any
       * other shape (returning a temp, a by-ref param, or a struct-returning
       * call result) is deferred to the fallback. */
      if (mir_type_is_indirect_aggregate(generator,
                                         function_data->return_type) &&
          !(in->lhs.kind == IR_OPERAND_SYMBOL &&
            mir_name_is_indirect_struct_local(generator, ir_function,
                                              in->lhs.name))) {
        return mir_trace_bail(function_data, "return:indirect_nonlocal");
      }
      break;
    case IR_OP_CALL:
      if (!mir_call_is_supported(generator, ir_function, in)) {
        return mir_trace_bail(function_data, "call_unsupported");
      }
      break;
    case IR_OP_CALL_INDIRECT:
      if (!mir_call_indirect_is_supported(generator, ir_function, in)) {
        return mir_trace_bail(function_data, "call_indirect_unsupported");
      }
      break;
    case IR_OP_ADDRESS_OF:
      /* &local/&param (made memory-resident via forced spill) or &global (kept
       * cached but coherent via flush/reload around pointer memory ops).
       * Functions/strings have their own address forms and are deferred. */
      if (mir_addressof_kind(generator, ir_function, in) ==
          MIR_ADDROF_UNSUPPORTED) {
        return mir_trace_bail(function_data, "addressof:unsupported");
      }
      if (in->dest.kind != IR_OPERAND_TEMP && in->dest.kind != IR_OPERAND_SYMBOL) {
        return mir_trace_bail(function_data, "addressof:dest");
      }
      break;
    case IR_OP_SIMD_SLP_MAC_I8:
    case IR_OP_SIMD_SLP_MAC_I32: {
      /* SLP MAC kernel run INLINE inside the MIR function (so the surrounding
       * outer loops keep register-allocated codegen). The lowering marshals the
       * three base pointers + count + byte stride into RCX/RDX/R8/R9/RAX, so the
       * only compile-time-constant requirement is the lane count K (it selects
       * the xmm-vs-ymm kernel width); the bases, offsets, count, and stride may
       * each be a runtime temp/symbol resolved via mir_value_operand. The I8
       * variant (int8 a/b, int32 c) uses the same shape with different element
       * scaling, handled in lowering. */
      if (in->argument_count < 6 || !in->arguments ||
          in->arguments[0].kind != IR_OPERAND_INT ||
          (in->arguments[0].int_value != 4 &&
           in->arguments[0].int_value != 8)) {
        return mir_trace_bail(function_data, "slp_mac:nonconst_K");
      }
      const IROperand *bases[3] = {&in->dest, &in->lhs, &in->rhs};
      for (int k = 0; k < 3; k++) {
        if (bases[k]->kind != IR_OPERAND_TEMP &&
            bases[k]->kind != IR_OPERAND_SYMBOL) {
          return mir_trace_bail(function_data, "slp_mac:base_kind");
        }
      }
      /* count, a_off, b_off, b_stride, out_off */
      const int run_args[5] = {1, 2, 3, 4, 5};
      for (int k = 0; k < 5; k++) {
        const IROperand *o = &in->arguments[run_args[k]];
        if (o->kind != IR_OPERAND_TEMP && o->kind != IR_OPERAND_SYMBOL &&
            o->kind != IR_OPERAND_INT) {
          return mir_trace_bail(function_data, "slp_mac:arg_kind");
        }
      }
      break;
    }
    case IR_OP_SIMD_FILL: {
      /* Inline fill passthrough, restricted to the element-counted (mode 0),
       * no-start, no-offset, no-live-iv-writeback subset with a compile-time
       * integer value -- the frame-clear / `a[i] = c` shape. Every other fill
       * form (byte-walk modes, runtime/float value, hoisted offset, or a live
       * induction variable that needs a final write-back) stays in the fallback,
       * which handles them all. */
      if (in->argument_count < 5 ||
          in->arguments[0].kind != IR_OPERAND_INT ||
          (in->arguments[0].int_value != 1 && in->arguments[0].int_value != 2 &&
           in->arguments[0].int_value != 4 &&
           in->arguments[0].int_value != 8) ||
          in->arguments[1].kind != IR_OPERAND_INT) {
        return mir_trace_bail(function_data, "simd_fill:shape");
      }
      /* Mode 0 (element-counted) and mode 1 (begin->end byte walk) only; mode 2
       * (byte-offset walk with a start and a live-iv write-back) defers. */
      long long fill_mode = in->arguments[1].int_value;
      if (fill_mode != 0 && fill_mode != 1) {
        return mir_trace_bail(function_data, "simd_fill:mode");
      }
      /* Mode 0 must start the induction variable at 0 (a nonzero start adjusts
       * both the base and the count; deferred). A nonzero/runtime OFFSET (the
       * invariant part of `base[offset + i]`) is supported by folding
       * `base + offset*size` in MIR before the kernel, but only for an int64
       * (wide) index so the pointer math is plain 64-bit -- an int32 offset would
       * need the fallback's sign-extension to match exactly. */
      if (fill_mode == 0) {
        if (!(in->arguments[3].kind == IR_OPERAND_INT &&
              in->arguments[3].int_value == 0)) {
          return mir_trace_bail(function_data, "simd_fill:start");
        }
        int offset_zero = (in->arguments[4].kind == IR_OPERAND_INT &&
                           in->arguments[4].int_value == 0);
        if (!offset_zero) {
          int wide = in->argument_count > 5 &&
                     in->arguments[5].kind == IR_OPERAND_INT &&
                     in->arguments[5].int_value == 64;
          if (!wide) {
            return mir_trace_bail(function_data, "simd_fill:offset_width");
          }
          if (in->arguments[4].kind != IR_OPERAND_TEMP &&
              in->arguments[4].kind != IR_OPERAND_SYMBOL &&
              in->arguments[4].kind != IR_OPERAND_INT) {
            return mir_trace_bail(function_data, "simd_fill:offset_kind");
          }
        }
      }
      /* A live induction variable (dest = the iv symbol) needs a final
       * write-back. Mode 0 with start 0 leaves iv = max(count, 0), folded in MIR
       * after the kernel -- but only when the iv is a LOCAL/PARAM (resolvable to
       * a vreg). Mode 1's pointer iv and a global iv stay in the fallback. */
      if (in->dest.kind == IR_OPERAND_SYMBOL) {
        if (fill_mode != 0 ||
            !mir_local_or_param_type(generator, ir_function, in->dest.name,
                                     NULL)) {
          return mir_trace_bail(function_data, "simd_fill:writeback");
        }
      }
      if (in->lhs.kind != IR_OPERAND_TEMP && in->lhs.kind != IR_OPERAND_SYMBOL) {
        return mir_trace_bail(function_data, "simd_fill:base");
      }
      if (in->rhs.kind != IR_OPERAND_TEMP && in->rhs.kind != IR_OPERAND_SYMBOL &&
          in->rhs.kind != IR_OPERAND_INT) {
        return mir_trace_bail(function_data, "simd_fill:count");
      }
      if (in->arguments[2].kind != IR_OPERAND_INT) {
        return mir_trace_bail(function_data, "simd_fill:value");
      }
      break;
    }
    case IR_OP_SIMD_AFFINE_MAP_F64:
    case IR_OP_SIMD_AFFINE_MAP_F32: {
      /* Inline float affine-map passthrough (`dst[i]=a*src[i]+b*dst[i]+c`, the
       * float-copy/saxpy class). src (lhs) and dst (rhs) must be LEA-able
       * pointers (TEMP/SYMBOL), the count GP-resolvable, and the a/b/c
       * coefficients compile-time FLOAT immediates (so the kernel can bake their
       * broadcasts); a runtime coefficient stays in the fallback. F32 and F64
       * share this validation; the lowering below picks the width. */
      if (in->argument_count < 4 || !in->arguments) {
        return mir_trace_bail(function_data, "affine_map:shape");
      }
      if ((in->lhs.kind != IR_OPERAND_TEMP && in->lhs.kind != IR_OPERAND_SYMBOL) ||
          (in->rhs.kind != IR_OPERAND_TEMP && in->rhs.kind != IR_OPERAND_SYMBOL)) {
        return mir_trace_bail(function_data, "affine_map:ptr");
      }
      if (in->arguments[0].kind != IR_OPERAND_TEMP &&
          in->arguments[0].kind != IR_OPERAND_SYMBOL &&
          in->arguments[0].kind != IR_OPERAND_INT) {
        return mir_trace_bail(function_data, "affine_map:count");
      }
      for (int k = 1; k <= 3; k++) {
        if (in->arguments[k].kind == IR_OPERAND_FLOAT) continue;
        /* F64 additionally accepts a RUNTIME `a` scale (arguments[1]) -- the
         * saxpy `y=a*x+y` shape where a varies per pass; it is marshalled into
         * an xmm and broadcast at runtime. b and c (args 2,3) must stay
         * compile-time so their broadcasts are baked. F32 stays const-only. */
        if (in->op == IR_OP_SIMD_AFFINE_MAP_F64 && k == 1 &&
            (in->arguments[k].kind == IR_OPERAND_TEMP ||
             in->arguments[k].kind == IR_OPERAND_SYMBOL)) {
          continue;
        }
        return mir_trace_bail(function_data, "affine_map:coeff");
      }
      break;
    }
    case IR_OP_SIMD_VLOOP_F64: {
      /* Inline general-vectorized-loop passthrough (float64 MAPS only). The DAG
       * is carried by reference at lowering; here we only gate the marshalling:
       * a map (not a reduction), float64 lanes, and <=3 distinct base pointers
       * so the element count still fits in an ABI arg register (RCX/RDX/R8/R9)
       * alongside them. Integer/float32 vloops and reductions stay in the
       * fallback. */
      const char *vnames[4];
      const IROperand *vsrcs[4];
      int vn = 0;
      if (in->argument_count < 7 || !in->arguments ||
          in->arguments[0].int_value != 0 /* reduce_op: maps only */ ||
          in->float_bits != 64) {
        return mir_trace_bail(function_data, "vloop:shape");
      }
      if (code_generator_vloop_collect_dist(in, 0, vnames, vsrcs, &vn) < 0 ||
          vn > 3) {
        return mir_trace_bail(function_data, "vloop:bases");
      }
      for (int vk = 0; vk < vn; vk++) {
        if (!vsrcs[vk] || (vsrcs[vk]->kind != IR_OPERAND_TEMP &&
                           vsrcs[vk]->kind != IR_OPERAND_SYMBOL)) {
          return mir_trace_bail(function_data, "vloop:ptr");
        }
      }
      if (in->lhs.kind != IR_OPERAND_TEMP && in->lhs.kind != IR_OPERAND_SYMBOL &&
          in->lhs.kind != IR_OPERAND_INT) {
        return mir_trace_bail(function_data, "vloop:count");
      }
      break;
    }
    case IR_OP_SIMD_SILU_F32: {
      /* Inline SiLU/SwiGLU passthrough. g (lhs) must be a LEA-able pointer, the
       * count GP-resolvable, and (SwiGLU) u (rhs) a pointer too; plain SiLU
       * leaves rhs NONE/"" (no multiply). */
      if (in->argument_count < 1 || !in->arguments ||
          (in->lhs.kind != IR_OPERAND_TEMP &&
           in->lhs.kind != IR_OPERAND_SYMBOL)) {
        return mir_trace_bail(function_data, "silu:g");
      }
      if (in->arguments[0].kind != IR_OPERAND_TEMP &&
          in->arguments[0].kind != IR_OPERAND_SYMBOL &&
          in->arguments[0].kind != IR_OPERAND_INT) {
        return mir_trace_bail(function_data, "silu:count");
      }
      if (in->rhs.kind != IR_OPERAND_NONE && in->rhs.kind != IR_OPERAND_STRING &&
          in->rhs.kind != IR_OPERAND_TEMP && in->rhs.kind != IR_OPERAND_SYMBOL) {
        return mir_trace_bail(function_data, "silu:u");
      }
      break;
    }
    default: {
      /* NEW, other SIMD ops, ROTATE_ADD: not yet. */
      char buf[40];
      snprintf(buf, sizeof(buf), "op:%d", (int)in->op);
      return mir_trace_bail(function_data, buf);
    }
    }
  }
  if (mir_env_trace()) {
    fprintf(stderr, "MIR-OK\t%s\n",
            function_data->name ? function_data->name : "?");
  }
  if (ir_explain_enabled() && function_data->name) {
    ir_explain_backend_function(function_data->name,
                                mir_function_filename(function_data), 1, NULL,
                                g_mir_gate_fn_size);
  }
  return 1;
}

/* ---- lowering ----------------------------------------------------------- */

static int mir_emit1(MirFunction *fn, MirOpcode op, MirOperand dst,
                     MirOperand a, MirOperand b, int width, int is_unsigned,
                     unsigned char cc) {
  MirInst in;
  memset(&in, 0, sizeof(in));
  in.op = op;
  in.dst = dst;
  in.a = a;
  in.b = b;
  in.width = width;
  in.is_unsigned = is_unsigned;
  in.cc = cc;
  in.ir_index = -1;
  return mir_emit(fn, &in);
}

/* ---- constant-divisor strength reduction (magic multiply) --------------- */

/* Granlund-Montgomery magic number for SIGNED 64-bit division by `d` (|d| >= 2).
 * The divide `n / d` becomes MULHS(n, M) [+/- n] >> s [+ sign bit]. (Hacker's
 * Delight, Fig. 10-1, widened to 64-bit.) */
static void mir_magic_s64(int64_t d, int64_t *Mout, int *sout) {
  const uint64_t two63 = 0x8000000000000000ULL;
  uint64_t ad = (uint64_t)(d < 0 ? -d : d);
  uint64_t t = two63 + ((uint64_t)d >> 63);
  uint64_t anc = t - 1 - t % ad; /* |nc| */
  int p = 63;
  uint64_t q1 = two63 / anc, r1 = two63 - q1 * anc;
  uint64_t q2 = two63 / ad, r2 = two63 - q2 * ad;
  uint64_t delta;
  do {
    p++;
    q1 <<= 1; r1 <<= 1;
    if (r1 >= anc) { q1++; r1 -= anc; }
    q2 <<= 1; r2 <<= 1;
    if (r2 >= ad) { q2++; r2 -= ad; }
    delta = ad - r2;
  } while (q1 < delta || (q1 == delta && r1 == 0));
  int64_t M = (int64_t)(q2 + 1);
  if (d < 0) M = -M;
  *Mout = M;
  *sout = p - 64;
}

/* Magic number for UNSIGNED 64-bit division by `d` (d >= 1, not a power of two).
 * `*addout` selects the overflow-safe reconstruction. (Hacker's Delight, Fig.
 * 10-3, widened to 64-bit.) */
static void mir_magic_u64(uint64_t d, uint64_t *Mout, int *sout, int *addout) {
  const uint64_t two63 = 0x8000000000000000ULL;
  *addout = 0;
  uint64_t nc = (uint64_t)(-1) - ((uint64_t)0 - d) % d;
  int p = 63;
  uint64_t q1 = two63 / nc, r1 = two63 - q1 * nc;
  uint64_t q2 = (two63 - 1) / d, r2 = (two63 - 1) - q2 * d;
  uint64_t delta;
  do {
    p++;
    if (r1 >= nc - r1) { q1 = 2 * q1 + 1; r1 = 2 * r1 - nc; }
    else { q1 = 2 * q1; r1 = 2 * r1; }
    if (r2 + 1 >= d - r2) {
      if (q2 >= two63 - 1) *addout = 1;
      q2 = 2 * q2 + 1; r2 = 2 * r2 + 1 - d;
    } else {
      if (q2 >= two63) *addout = 1;
      q2 = 2 * q2; r2 = 2 * r2 + 1;
    }
    delta = d - 1 - r2;
  } while (p < 128 && (q1 < delta || (q1 == delta && r1 == 0)));
  *Mout = q2 + 1;
  *sout = p - 64;
}

/* The pooled GP vreg for a loop-invariant 64-bit integer constant, or NONE. */
static MirVregId mir_iconst_lookup(MirFunction *fn, int64_t value) {
  for (size_t i = 0; i < fn->iconst_count; i++) {
    if (fn->iconsts[i].value == value) {
      return fn->iconsts[i].vreg;
    }
  }
  return MIR_VREG_NONE;
}

/* Add `value` to the integer-constant pool and emit its initial materialization.
 * A later MIR layout pass moves the movabs to a hot-loop preheader. No-op if
 * already pooled. */
static int mir_iconst_add(MirFunction *fn, int64_t value) {
  if (mir_iconst_lookup(fn, value) != MIR_VREG_NONE) {
    return 1;
  }
  if (fn->iconst_count >= fn->iconst_capacity) {
    size_t nc = fn->iconst_capacity ? fn->iconst_capacity * 2 : 8;
    MirIConst *grown =
        (MirIConst *)realloc(fn->iconsts, nc * sizeof(MirIConst));
    if (!grown) {
      fn->has_error = 1;
      return 0;
    }
    fn->iconsts = grown;
    fn->iconst_capacity = nc;
  }
  MirVregId v = mir_new_vreg(fn, MIR_RC_GP, 8);
  if (v == MIR_VREG_NONE) {
    return 0;
  }
  fn->iconsts[fn->iconst_count].value = value;
  fn->iconsts[fn->iconst_count].vreg = v;
  fn->iconst_count++;
  return mir_emit1(fn, MIR_MOV, mir_op_vreg(v), mir_op_imm(value),
                   mir_op_none(), 8, 0, 0);
}

/* An integer-constant operand: the hoisted pool vreg if `value` was pooled,
 * otherwise an inline immediate. */
static MirOperand mir_iconst_operand(MirFunction *fn, int64_t value) {
  MirVregId v = mir_iconst_lookup(fn, value);
  return (v != MIR_VREG_NONE) ? mir_op_vreg(v) : mir_op_imm(value);
}

/* If `a / C` or `a % C` (compile-time constant C, dividend signedness `uns`)
 * lowers via a magic-multiply MULHI, return 1 and set *Mout to the 64-bit magic
 * constant the MULHI multiplies by; return 0 for the forms that emit no MULHI
 * (C in {0, 1, -1} or |C| a power of two). Mirrors the magic selection inside
 * mir_emit_const_divmod so the magic can be pre-pooled and hoisted out of a
 * loop. */
static int mir_divmod_magic(int64_t C, int uns, int64_t *Mout) {
  if (C == 0 || C == 1 || (!uns && C == -1)) {
    return 0;
  }
  uint64_t ad = uns ? (uint64_t)C : (uint64_t)(C < 0 ? -C : C);
  if ((ad & (ad - 1)) == 0) {
    return 0; /* power of two -> shift, no MULHI */
  }
  if (uns) {
    uint64_t M;
    int s, add;
    mir_magic_u64(ad, &M, &s, &add);
    *Mout = (int64_t)M;
  } else {
    int64_t M;
    int s;
    mir_magic_s64(C, &M, &s);
    *Mout = M;
  }
  return 1;
}

/* Strength-reduce `dst = a / C` or `dst = a % C` for a compile-time constant C
 * into a magic-number multiply (+ shifts), avoiding the long-latency divide.
 * Returns 1 if it emitted the reduced form, 0 to fall back to a real divide
 * (C == 0 keeps the divide so the /0 runtime trap fires). `uns` is the
 * dividend's signedness; `mod` selects remainder. All math is 64-bit. */
static int mir_emit_const_divmod(MirFunction *fn, MirOperand dst, MirOperand a,
                                 int64_t C, int uns, int mod) {
  if (C == 0) {
    return 0;
  }
  /* The dividend is read repeatedly (MULHI, then the remainder/sign-correction
   * ops). A vreg is safe to re-read directly: it never lives in RAX/RDX (those
   * are non-allocatable encoder scratch), so MULHI's RAX:RDX clobber cannot
   * corrupt it, and this function never writes `a` before its last read. Only an
   * immediate (or other non-vreg) needs a fresh-vreg snapshot — copying it once
   * avoids re-emitting a 10-byte movabs at each use. Skipping the copy for the
   * common register dividend removes one mov per div/mod in hot loops. */
  MirOperand A;
  if (a.kind == MIR_OPK_VREG) {
    A = a;
  } else {
    MirVregId av = mir_new_vreg(fn, MIR_RC_GP, 8);
    if (av == MIR_VREG_NONE ||
        !mir_emit1(fn, MIR_MOV, mir_op_vreg(av), a, mir_op_none(), 8, 0, 0)) {
      return 0;
    }
    A = mir_op_vreg(av);
  }

  if (C == 1) {
    return mir_emit1(fn, MIR_MOV, dst, mod ? mir_op_imm(0) : A, mir_op_none(), 8,
                     0, 0);
  }
  if (!uns && C == -1) {
    if (mod) {
      return mir_emit1(fn, MIR_MOV, dst, mir_op_imm(0), mir_op_none(), 8, 0, 0);
    }
    return mir_emit1(fn, MIR_NEG, dst, A, mir_op_none(), 8, 0, 0);
  }

  uint64_t ad = uns ? (uint64_t)C : (uint64_t)(C < 0 ? -C : C);
  int is_pow2 = (ad & (ad - 1)) == 0;
  int k = 0;
  for (uint64_t tt = ad; tt > 1; tt >>= 1) {
    k++;
  }

  int q_in_dst = !mod && dst.kind == MIR_OPK_VREG &&
                 !(A.kind == MIR_OPK_VREG && A.vreg == dst.vreg);
  MirVregId qv = q_in_dst ? dst.vreg : mir_new_vreg(fn, MIR_RC_GP, 8);
  if (qv == MIR_VREG_NONE) {
    return 0;
  }
  MirOperand Q = q_in_dst ? dst : mir_op_vreg(qv);

  if (is_pow2) {
    if (uns) {
      if (!mir_emit1(fn, MIR_SHR, Q, A, mir_op_imm(k), 8, 1, 0)) {
        return 0;
      }
    } else {
      /* bias = (a < 0) ? (2^k - 1) : 0 ; q = (a + bias) >> k (arithmetic). */
      MirVregId t1 = mir_new_vreg(fn, MIR_RC_GP, 8);
      MirVregId t2 = mir_new_vreg(fn, MIR_RC_GP, 8);
      if (t1 == MIR_VREG_NONE || t2 == MIR_VREG_NONE ||
          !mir_emit1(fn, MIR_SAR, mir_op_vreg(t1), A, mir_op_imm(63), 8, 0, 0) ||
          !mir_emit1(fn, MIR_SHR, mir_op_vreg(t2), mir_op_vreg(t1),
                     mir_op_imm(64 - k), 8, 1, 0) ||
          !mir_emit1(fn, MIR_ADD, Q, A, mir_op_vreg(t2), 8, 0, 0) ||
          !mir_emit1(fn, MIR_SAR, Q, Q, mir_op_imm(k), 8, 0, 0)) {
        return 0;
      }
      if (C < 0 && !mir_emit1(fn, MIR_NEG, Q, Q, mir_op_none(), 8, 0, 0)) {
        return 0;
      }
    }
  } else if (uns) {
    uint64_t M;
    int s, add;
    mir_magic_u64(ad, &M, &s, &add);
    MirVregId tv = mir_new_vreg(fn, MIR_RC_GP, 8);
    if (tv == MIR_VREG_NONE ||
        !mir_emit1(fn, MIR_MULHI, mir_op_vreg(tv), A,
                   mir_iconst_operand(fn, (int64_t)M), 8, 1, 0)) {
      return 0;
    }
    if (!add) {
      if (!mir_emit1(fn, MIR_SHR, Q, mir_op_vreg(tv), mir_op_imm(s), 8, 1, 0)) {
        return 0;
      }
    } else {
      /* q = (((a - t) >> 1) + t) >> (s - 1)  (overflow-safe average). */
      MirVregId d1 = mir_new_vreg(fn, MIR_RC_GP, 8);
      if (d1 == MIR_VREG_NONE ||
          !mir_emit1(fn, MIR_SUB, mir_op_vreg(d1), A, mir_op_vreg(tv), 8, 0,
                     0) ||
          !mir_emit1(fn, MIR_SHR, mir_op_vreg(d1), mir_op_vreg(d1),
                     mir_op_imm(1), 8, 1, 0) ||
          !mir_emit1(fn, MIR_ADD, mir_op_vreg(d1), mir_op_vreg(d1),
                     mir_op_vreg(tv), 8, 0, 0) ||
          !mir_emit1(fn, MIR_SHR, Q, mir_op_vreg(d1), mir_op_imm(s - 1), 8, 1,
                     0)) {
        return 0;
      }
    }
  } else {
    int64_t M;
    int s;
    mir_magic_s64(C, &M, &s);
    if (!mir_emit1(fn, MIR_MULHI, Q, A, mir_iconst_operand(fn, M), 8, 0, 0)) {
      return 0;
    }
    if (C > 0 && M < 0) {
      if (!mir_emit1(fn, MIR_ADD, Q, Q, A, 8, 0, 0)) {
        return 0;
      }
    } else if (C < 0 && M > 0) {
      if (!mir_emit1(fn, MIR_SUB, Q, Q, A, 8, 0, 0)) {
        return 0;
      }
    }
    if (s > 0 && !mir_emit1(fn, MIR_SAR, Q, Q, mir_op_imm(s), 8, 0, 0)) {
      return 0;
    }
    /* q += sign bit of q (round toward zero). */
    MirVregId sb = mir_new_vreg(fn, MIR_RC_GP, 8);
    if (sb == MIR_VREG_NONE ||
        !mir_emit1(fn, MIR_SHR, mir_op_vreg(sb), Q, mir_op_imm(63), 8, 1, 0) ||
        !mir_emit1(fn, MIR_ADD, Q, Q, mir_op_vreg(sb), 8, 0, 0)) {
      return 0;
    }
  }

  if (!mod) {
    return q_in_dst ? 1
                    : mir_emit1(fn, MIR_MOV, dst, Q, mir_op_none(), 8, 0, 0);
  }
  /* remainder = a - q * C */
  MirVregId mv = mir_new_vreg(fn, MIR_RC_GP, 8);
  if (mv == MIR_VREG_NONE) {
    return 0;
  }
  if (C >= INT32_MIN && C <= INT32_MAX) {
    if (!mir_emit1(fn, MIR_IMUL, mir_op_vreg(mv), Q, mir_op_imm(C), 8, 0, 0)) {
      return 0;
    }
  } else {
    MirVregId cv = mir_new_vreg(fn, MIR_RC_GP, 8);
    if (cv == MIR_VREG_NONE ||
        !mir_emit1(fn, MIR_MOV, mir_op_vreg(cv), mir_op_imm(C), mir_op_none(), 8,
                   0, 0) ||
        !mir_emit1(fn, MIR_IMUL, mir_op_vreg(mv), Q, mir_op_vreg(cv), 8, 0, 0)) {
      return 0;
    }
  }
  return mir_emit1(fn, MIR_SUB, dst, A, mir_op_vreg(mv), 8, 0, 0);
}

/* Emit a MIR_STORE_GLOBAL for each named global, writing its cached vreg back to
 * memory (Vg -> [g]). */
static int mir_emit_global_flush_names(MirFunction *fn, CodeGenerator *g,
                                       MirNameMap *map, const char **names,
                                       size_t count) {
  for (size_t i = 0; i < count; i++) {
    const char *name = names[i];
    Symbol *s = symbol_table_lookup(g->symbol_table, name);
    int size = s ? code_generator_binary_resolved_type_scalar_size(s->type) : 0;
    if (size != 1 && size != 2 && size != 4 && size != 8) {
      fn->has_error = 1;
      return 0;
    }
    MirVregId v = mir_name_map_get_or_add(map, fn, name, 0, MIR_RC_GP, 8);
    if (v == MIR_VREG_NONE) {
      return 0;
    }
    if (!mir_emit1(fn, MIR_STORE_GLOBAL, mir_op_none(), mir_op_symbol(name),
                   mir_op_vreg(v), size, 0, 0)) {
      return 0;
    }
  }
  return 1;
}

/* Emit a MIR_LOAD_GLOBAL for each named global, refreshing its cache vreg from
 * memory ([g] -> Vg). */
static int mir_emit_global_reload_names(MirFunction *fn, CodeGenerator *g,
                                        MirNameMap *map, const char **names,
                                        size_t count) {
  for (size_t i = 0; i < count; i++) {
    const char *name = names[i];
    Symbol *s = symbol_table_lookup(g->symbol_table, name);
    int size = s ? code_generator_binary_resolved_type_scalar_size(s->type) : 0;
    if (size != 1 && size != 2 && size != 4 && size != 8) {
      fn->has_error = 1;
      return 0;
    }
    int is_signed =
        code_generator_binary_resolved_type_is_signed_integer(s->type);
    int fbits = code_generator_binary_resolved_type_float_bits(s->type);
    MirVregId v = mir_name_map_get_or_add(map, fn, name, 0,
                                          fbits ? MIR_RC_XMM : MIR_RC_GP,
                                          fbits ? fbits / 8 : 8);
    if (v == MIR_VREG_NONE) {
      return 0;
    }
    if (!mir_emit1(fn, MIR_LOAD_GLOBAL, mir_op_vreg(v), mir_op_symbol(name),
                   mir_op_none(), size, is_signed ? 0 : 1, 0)) {
      return 0;
    }
  }
  return 1;
}

/* Flush the written cached globals back to memory. Called before each MIR_RET
 * (so memory is consistent on every exit) and before a call (so the callee sees
 * current values). */
static int mir_emit_global_writebacks(MirFunction *fn, CodeGenerator *g,
                                      MirNameMap *map,
                                      const MirGlobalWriteback *wb) {
  if (!wb) {
    return 1;
  }
  return mir_emit_global_flush_names(fn, g, map, wb->names, wb->count);
}

/* Reload every cached global EXCEPT `except` (borrowed name). Used after a call
 * whose result is assigned straight to a global (`@g = f()`, which the optimizer
 * fuses into one CALL with dest=@g): the call lowering has already captured the
 * return value into @g's cache vreg, and C semantics discard any write the
 * callee made to @g's memory, so reloading @g from (still-stale) memory would
 * wrongly clobber the fresh result with the old value. */
static int mir_emit_global_reloads_except(MirFunction *fn, CodeGenerator *g,
                                          MirNameMap *map,
                                          const MirGlobalWriteback *wb,
                                          const char *except) {
  if (!wb) {
    return 1;
  }
  for (size_t i = 0; i < wb->all_count; i++) {
    if (except && wb->all[i] && strcmp(wb->all[i], except) == 0) {
      continue;
    }
    if (!mir_emit_global_reload_names(fn, g, map, &wb->all[i], 1)) {
      return 0;
    }
  }
  return 1;
}

static int mir_instruction_writes_symbol(const IRInstruction *in,
                                         const char *name) {
  if (!in || in->op == IR_OP_DECLARE_LOCAL || in->op == IR_OP_NOP) {
    return 0;
  }
  return name && in->dest.kind == IR_OPERAND_SYMBOL && in->dest.name &&
         strcmp(in->dest.name, name) == 0;
}

static int mir_address_of_function_target(CodeGenerator *g,
                                          const IROperand *op) {
  if (!g || !op || op->kind != IR_OPERAND_SYMBOL || !op->name) {
    return 0;
  }
  Symbol *s = g->symbol_table ? symbol_table_lookup(g->symbol_table, op->name)
                              : NULL;
  return (s && s->kind == SYMBOL_FUNCTION) ||
         mir_find_ir_function_named(g, op->name) != NULL;
}

static const char *mir_known_function_pointer_target(CodeGenerator *g,
                                                     const IRFunction *irf,
                                                     size_t before,
                                                     const char *name) {
  if (!g || !irf || !name) {
    return NULL;
  }
  int is_param = 0;
  Type *ft = mir_local_or_param_type(g, irf, name, &is_param);
  if (!ft || is_param || ft->kind != TYPE_FUNCTION_POINTER) {
    return NULL;
  }

  const char *target = NULL;
  int writes = 0;
  for (size_t i = 0; i < before && i < irf->instruction_count; i++) {
    const IRInstruction *in = &irf->instructions[i];
    if (!mir_instruction_writes_symbol(in, name)) {
      continue;
    }
    writes++;
    if (writes > 1 || in->op != IR_OP_ADDRESS_OF ||
        !mir_address_of_function_target(g, &in->lhs)) {
      return NULL;
    }
    target = in->lhs.name;
  }
  return writes == 1 ? target : NULL;
}

static int mir_ir_function_may_write_global(CodeGenerator *g,
                                            const IRFunction *irf) {
  if (!g || !irf) {
    return 1;
  }
  for (size_t i = 0; i < irf->instruction_count; i++) {
    const IRInstruction *in = &irf->instructions[i];
    switch (in->op) {
    case IR_OP_CALL:
    case IR_OP_CALL_INDIRECT:
    case IR_OP_INLINE_ASM:
    case IR_OP_NEW:
    case IR_OP_STORE:
      return 1;
    default:
      break;
    }
    if (in->dest.kind == IR_OPERAND_SYMBOL && in->dest.name &&
        !mir_local_or_param_type(g, irf, in->dest.name, NULL) &&
        mir_name_is_global_scalar(g, in->dest.name)) {
      return 1;
    }
  }
  return 0;
}

static int mir_call_may_write_globals(CodeGenerator *g, const IRFunction *irf,
                                      size_t index,
                                      const IRInstruction *in) {
  if (!g || !irf || !in) {
    return 1;
  }
  const char *target = NULL;
  if (in->op == IR_OP_CALL) {
    target = in->text;
  } else if (in->op == IR_OP_CALL_INDIRECT &&
             in->lhs.kind == IR_OPERAND_SYMBOL && in->lhs.name) {
    target = mir_known_function_pointer_target(g, irf, index, in->lhs.name);
  }
  if (!target || !target[0]) {
    return 1;
  }
  IRFunction *target_ir = mir_find_ir_function_named(g, target);
  return !target_ir || mir_ir_function_may_write_global(g, target_ir);
}

/* Emit a fixed-size byte copy of `size` bytes from [src_base] to [dst_base],
 * where both bases are pointer vregs. Lowered as a straight-line sequence of
 * load/store pairs through a fresh GP temp (8 bytes at a time, then a 4/2/1
 * tail) — exactly the [base + disp] memory MOVs the field-access path already
 * uses, so it needs no new encoder support and the allocator schedules the
 * pointers and temps normally. Used to copy an INDIRECT struct into a caller's
 * hidden return slot (and, later, for whole-struct assignment and arguments). */
static int mir_emit_struct_copy(MirFunction *fn, MirVregId dst_base,
                                MirVregId src_base, int size) {
  for (int k = 0; k < size;) {
    int rem = size - k;
    int w = rem >= 8 ? 8 : (rem >= 4 ? 4 : (rem >= 2 ? 2 : 1));
    MirVregId tmp = mir_new_vreg(fn, MIR_RC_GP, 8);
    if (tmp == MIR_VREG_NONE) {
      return 0;
    }
    MirOperand src_mem = mir_op_mem_vreg(src_base, MIR_VREG_NONE, 1, k);
    MirOperand dst_mem = mir_op_mem_vreg(dst_base, MIR_VREG_NONE, 1, k);
    if (!mir_emit1(fn, MIR_MOV, mir_op_vreg(tmp), src_mem, mir_op_none(), w, 1,
                   0) ||
        !mir_emit1(fn, MIR_MOV, dst_mem, mir_op_vreg(tmp), mir_op_none(), w, 1,
                   0)) {
      return 0;
    }
    k += w;
  }
  return 1;
}

/* A width-tagged float register move (xmm copy). */
static int mir_emit_fmov(MirFunction *fn, MirOperand dst, MirOperand src,
                         int width) {
  MirInst in;
  memset(&in, 0, sizeof(in));
  in.op = MIR_MOV;
  in.is_float = 1;
  in.dst = dst;
  in.a = src;
  in.width = width;
  in.ir_index = -1;
  return mir_emit(fn, &in);
}

/* Raw IEEE-754 bits of a double value at the given float width (4 or 8). */
static uint64_t mir_float_bits_at(double value, int width_bytes) {
  if (width_bytes == 4) {
    float f = (float)value;
    uint32_t u;
    memcpy(&u, &f, sizeof(u));
    return u;
  }
  uint64_t u;
  memcpy(&u, &value, sizeof(u));
  return u;
}

/* The pooled vreg for a loop-invariant constant (bits,width), or MIR_VREG_NONE. */
static MirVregId mir_pool_lookup(MirFunction *fn, uint64_t bits, int width) {
  for (size_t i = 0; i < fn->fconst_count; i++) {
    if (fn->fconsts[i].bits == bits && fn->fconsts[i].width == width) {
      return fn->fconsts[i].vreg;
    }
  }
  return MIR_VREG_NONE;
}

/* Add (bits,width) to the float-constant pool and emit its initial
 * materialization.
 * A later MIR layout pass moves it to a hot-loop preheader. No-op if already
 * pooled. */
static int mir_pool_add(MirFunction *fn, uint64_t bits, int width) {
  if (mir_pool_lookup(fn, bits, width) != MIR_VREG_NONE) {
    return 1;
  }
  if (fn->fconst_count >= fn->fconst_capacity) {
    size_t nc = fn->fconst_capacity ? fn->fconst_capacity * 2 : 8;
    MirFConst *grown =
        (MirFConst *)realloc(fn->fconsts, nc * sizeof(MirFConst));
    if (!grown) {
      fn->has_error = 1;
      return 0;
    }
    fn->fconsts = grown;
    fn->fconst_capacity = nc;
  }
  MirVregId v = mir_new_vreg(fn, MIR_RC_XMM, width);
  if (v == MIR_VREG_NONE) {
    return 0;
  }
  fn->fconsts[fn->fconst_count].bits = bits;
  fn->fconsts[fn->fconst_count].width = width;
  fn->fconsts[fn->fconst_count].vreg = v;
  fn->fconst_count++;
  return mir_emit_fmov(fn, mir_op_vreg(v), mir_op_fimm(bits), width);
}

/* A float-constant operand: the hoisted pool vreg if this (value,width) was
 * pooled, otherwise an inline float immediate. */
static MirOperand mir_float_const_operand(MirFunction *fn, double value,
                                          int width) {
  uint64_t bits = mir_float_bits_at(value, width);
  MirVregId v = mir_pool_lookup(fn, bits, width);
  return (v != MIR_VREG_NONE) ? mir_op_vreg(v) : mir_op_fimm(bits);
}

/* Resolve a float operand to the operation's width `target_bytes`, inserting a
 * cvtss2sd/cvtsd2ss when the operand's natural float width differs. A float
 * literal is materialized directly at the target width. This is the implicit
 * promotion/narrowing the IR leaves to the backend (e.g. float32 * 1.5 computes
 * at float64). */
static MirOperand coerce_float_operand(MirFunction *fn, CodeGenerator *g,
                                       BinaryFunctionContext *ctx,
                                       MirNameMap *map, const IROperand *op,
                                       int target_bytes) {
  if (op->kind == IR_OPERAND_FLOAT) {
    return mir_float_const_operand(fn, op->float_value, target_bytes);
  }
  if (op->kind == IR_OPERAND_INT) {
    /* Integer literal used in a float op -> a float constant of that value. */
    return mir_float_const_operand(fn, (double)op->int_value, target_bytes);
  }
  MirOperand v = mir_value_operand(fn, g, ctx, map, op);
  int fb = code_generator_binary_operand_float_bits(g, ctx, op);
  if (fb == 0) {
    /* Integer operand promoted into a float op (the IR leaves the cvtsi2sd to
     * the backend, e.g. `f + y` with y an int). */
    MirVregId tmp = mir_new_vreg(fn, MIR_RC_XMM, target_bytes);
    if (tmp == MIR_VREG_NONE) {
      return v;
    }
    mir_emit1(fn, MIR_CVTSI2F, mir_op_vreg(tmp), v, mir_op_none(), target_bytes,
              0, 0);
    return mir_op_vreg(tmp);
  }
  if (fb / 8 != target_bytes) {
    MirVregId tmp = mir_new_vreg(fn, MIR_RC_XMM, target_bytes);
    if (tmp == MIR_VREG_NONE) {
      return v;
    }
    mir_emit1(fn, MIR_CVTF2F, mir_op_vreg(tmp), v, mir_op_none(), target_bytes,
              0, 0);
    return mir_op_vreg(tmp);
  }
  return v;
}

/* Operand (compute) width in bytes of a float comparison's operands. */
static int mir_float_cmp_width(CodeGenerator *g, BinaryFunctionContext *ctx,
                               const IRInstruction *in) {
  int fb = code_generator_binary_operand_float_bits(g, ctx, &in->lhs);
  if (!fb) {
    fb = code_generator_binary_operand_float_bits(g, ctx, &in->rhs);
  }
  return fb ? fb / 8 : 8;
}

/* IR index of a label definition by name, or SIZE_MAX. */
static size_t mir_ir_label_index(IRFunction *function, const char *name) {
  if (!name) {
    return SIZE_MAX;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op == IR_OP_LABEL && in->text && strcmp(in->text, name) == 0) {
      return i;
    }
  }
  return SIZE_MAX;
}

/* Build the constant pools: every distinct float literal used INSIDE a loop (a
 * backward jump/branch range), plus the 64-bit magic-multiply constant of every
 * in-loop `x / C` / `x % C` with a constant divisor, is hoisted to a vreg.
 * Constants outside loops are left inline (no register-pressure benefit). Must
 * run before the body is lowered so uses can resolve to pooled vregs; the
 * materializations are relocated after MIR layout. */
static int mir_build_const_pool(MirFunction *fn, CodeGenerator *g,
                                BinaryFunctionContext *ctx,
                                IRFunction *function) {
  size_t n = function->instruction_count;
  if (n == 0) {
    return 1;
  }
  char *in_loop = (char *)calloc(n, 1);
  if (!in_loop) {
    fn->has_error = 1;
    return 0;
  }
  for (size_t j = 0; j < n; j++) {
    const IRInstruction *in = &function->instructions[j];
    const char *target = (in->op == IR_OP_JUMP || in->op == IR_OP_BRANCH_ZERO)
                             ? in->text
                             : NULL;
    if (!target) {
      continue;
    }
    size_t l = mir_ir_label_index(function, target);
    if (l != SIZE_MAX && l < j) {
      for (size_t k = l; k <= j; k++) {
        in_loop[k] = 1;
      }
    }
  }

  int ok = 1;
  for (size_t j = 0; j < n && ok; j++) {
    if (!in_loop[j]) {
      continue;
    }
    const IRInstruction *in = &function->instructions[j];
    /* Pool the div/mod magic-multiply constant for `x / C` / `x % C` (a
     * compile-time-constant divisor) so the 64-bit magic is materialized once at
     * preheader instead of with a 10-byte movabs every loop iteration. */
    if (in->op == IR_OP_BINARY && !in->is_float && in->text &&
        (in->text[0] == '/' || in->text[0] == '%') && in->text[1] == '\0' &&
        in->rhs.kind == IR_OPERAND_INT) {
      int uns = mir_operand_is_unsigned(g, ctx, &in->lhs);
      int64_t M;
      if (mir_divmod_magic(in->rhs.int_value, uns, &M) && !mir_iconst_add(fn, M)) {
        ok = 0;
        break;
      }
    }
    const IROperand *ops[2] = {NULL, NULL};
    int w = 0;
    if (in->op == IR_OP_BINARY && in->is_float) {
      int fb = code_generator_binary_instruction_result_float_bits(g, ctx, in);
      w = fb ? fb / 8 : 8;
      ops[0] = &in->lhs;
      ops[1] = &in->rhs;
    } else if (in->op == IR_OP_ASSIGN) {
      int fb = code_generator_binary_operand_float_bits(g, ctx, &in->dest);
      if (fb) {
        w = fb / 8;
        ops[0] = &in->lhs;
      }
    }
    if (!w) {
      continue;
    }
    for (int k = 0; k < 2; k++) {
      if (ops[k] && ops[k]->kind == IR_OPERAND_FLOAT) {
        if (!mir_pool_add(fn, mir_float_bits_at(ops[k]->float_value, w), w)) {
          ok = 0;
          break;
        }
      }
    }
  }
  free(in_loop);
  return ok;
}

/* If `op` is an integer constant usable as a 32-bit compare immediate, return 1
 * and set *out to its sign-extended value. Recognizes a literal INT directly, or
 * a temp whose single definition is a CAST of an integer literal to an integer
 * type (the shape a loop bound like `i < (int64)N` takes). The cast value is
 * recomputed at the destination width/signedness so a narrowing cast cannot fold
 * to the wrong number, and only values fitting signed-32 are accepted. This lets
 * a counted-loop bound become `cmp reg, imm32` instead of being rematerialized
 * into a register every iteration. */
static int mir_fused_cmp_imm(CodeGenerator *g, BinaryFunctionContext *ctx,
                             const IRFunction *f, const IROperand *op,
                             long long *out) {
  long long v;
  if (op->kind == IR_OPERAND_INT) {
    v = op->int_value;
  } else if (op->kind == IR_OPERAND_TEMP && op->name) {
    const IRInstruction *def = NULL;
    int defs = 0;
    for (size_t i = 0; i < f->instruction_count; i++) {
      const IRInstruction *in = &f->instructions[i];
      if (in->dest.kind == IR_OPERAND_TEMP && in->dest.name &&
          strcmp(in->dest.name, op->name) == 0) {
        def = in;
        defs++;
      }
    }
    if (defs != 1 || !def || def->op != IR_OP_CAST ||
        def->lhs.kind != IR_OPERAND_INT || !def->text) {
      return 0;
    }
    /* The cast target type is named by def->text (e.g. "int64"); the temp's dest
     * type is not registered in this context, so resolve from the name. */
    Type *dt = code_generator_binary_get_resolved_type(g, def->text, 0);
    if (!dt || code_generator_binary_resolved_type_float_bits(dt)) {
      return 0;
    }
    (void)ctx;
    int sz = code_generator_binary_resolved_type_scalar_size(dt);
    int sgn = code_generator_binary_resolved_type_is_signed_integer(dt);
    v = def->lhs.int_value;
    if (sz == 1) {
      v = sgn ? (long long)(signed char)v : (long long)(unsigned char)v;
    } else if (sz == 2) {
      v = sgn ? (long long)(short)v : (long long)(unsigned short)v;
    } else if (sz == 4) {
      v = sgn ? (long long)(int)v : (long long)(unsigned int)v;
    } else if (sz != 8) {
      return 0;
    }
  } else {
    return 0;
  }
  if (v < -2147483648LL || v > 2147483647LL) {
    return 0;
  }
  *out = v;
  return 1;
}

/* Fuse `%t = a CMP b; branch_zero %t -> L` into a compare-and-branch: integer
 * `cmp a,b; j<!CMP> L`, or float `ucomis a,b; j<!CMP> L`. */
static int mir_lower_compare_branch(MirFunction *fn, CodeGenerator *g,
                                    BinaryFunctionContext *ctx, MirNameMap *map,
                                    const IRFunction *ir_function,
                                    const IRInstruction *cmp,
                                    const IRInstruction *br) {
  if (cmp->is_float) {
    int swap;
    unsigned char cc = 0;
    if (!mir_float_cmp_info(cmp->text, 1, &swap, &cc)) {
      fn->has_error = 1;
      return 0;
    }
    int w = mir_float_cmp_width(g, ctx, cmp);
    const IROperand *lo = swap ? &cmp->rhs : &cmp->lhs;
    const IROperand *ro = swap ? &cmp->lhs : &cmp->rhs;
    MirOperand a = coerce_float_operand(fn, g, ctx, map, lo, w);
    MirOperand b = coerce_float_operand(fn, g, ctx, map, ro, w);
    return mir_emit1(fn, MIR_FCMPBR, mir_op_label(br->text), a, b, w, 0, cc);
  }
  MirOperand a = mir_value_operand(fn, g, ctx, map, &cmp->lhs);
  int uns = mir_operand_is_unsigned(g, ctx, &cmp->lhs) ||
            mir_operand_is_unsigned(g, ctx, &cmp->rhs);
  /* Fold a constant right-hand bound into the compare as an imm32 so the loop
   * does not rematerialize it into a register every iteration. The producer is
   * dropped separately (mir_compute_const_compare_skips). */
  long long imm;
  MirOperand b;
  if (mir_fused_cmp_imm(g, ctx, ir_function, &cmp->rhs, &imm)) {
    b = mir_op_imm(imm);
  } else {
    b = mir_value_operand(fn, g, ctx, map, &cmp->rhs);
  }
  unsigned char cc = 0;
  if (!mir_false_jcc(cmp->text, uns, &cc)) {
    fn->has_error = 1;
    return 0;
  }
  int w = mir_int_compare_width(g, ctx, cmp->text, &cmp->lhs, &cmp->rhs);
  return mir_emit1(fn, MIR_CMPBR, mir_op_label(br->text), a, b, w, uns, cc);
}

/* True when instruction i is a single-use comparison (integer or ordered float)
 * whose result is consumed only by an immediately-following branch_zero. */
static int mir_fuses_compare_branch(CodeGenerator *g, IRFunction *function,
                                    size_t i) {
  if (i + 1 >= function->instruction_count) {
    return 0;
  }
  const IRInstruction *cmp = &function->instructions[i];
  const IRInstruction *br = &function->instructions[i + 1];
  if (cmp->op != IR_OP_BINARY || !cmp->text ||
      cmp->dest.kind != IR_OPERAND_TEMP || !cmp->dest.name) {
    return 0;
  }
  int sw;
  unsigned char fcc;
  int ok_cmp = cmp->is_float ? mir_float_cmp_info(cmp->text, 1, &sw, &fcc)
                             : mir_is_comparison(cmp->text);
  if (!ok_cmp) {
    return 0;
  }
  if (br->op != IR_OP_BRANCH_ZERO || br->lhs.kind != IR_OPERAND_TEMP ||
      !br->lhs.name || strcmp(br->lhs.name, cmp->dest.name) != 0) {
    return 0;
  }
  (void)g;
  return code_generator_binary_function_temp_use_count(function,
                                                       cmp->dest.name) == 1;
}

static int mir_lower_instruction(MirFunction *fn, CodeGenerator *g,
                                 BinaryFunctionContext *ctx, MirNameMap *map,
                                 const IRInstruction *in,
                                 const MirGlobalWriteback *wb) {
  switch (in->op) {
  case IR_OP_NOP:
  case IR_OP_DECLARE_LOCAL:
    return 1;

  case IR_OP_LABEL:
    return mir_emit1(fn, MIR_LABEL, mir_op_label(in->text), mir_op_none(),
                     mir_op_none(), 8, 0, 0);

  case IR_OP_JUMP:
    return mir_emit1(fn, MIR_JMP, mir_op_label(in->text), mir_op_none(),
                     mir_op_none(), 8, 0, 0);

  case IR_OP_BRANCH_ZERO: {
    /* if (cond == 0) goto label  ->  test cond; je label */
    MirOperand cond = mir_value_operand(fn, g, ctx, map, &in->lhs);
    return mir_emit1(fn, MIR_JCC, mir_op_label(in->text), cond, mir_op_none(), 8,
                     0, 0x84 /* je */);
  }

  case IR_OP_BRANCH_EQ: {
    /* if (lhs == rhs) goto label  ->  cmp lhs,rhs; je label. Equality, so
     * signedness is irrelevant and a constant rhs (the common switch/match
     * case value) folds into the cmp's imm32 (or a scratch reg if it doesn't
     * fit) inside the MIR_CMPBR encoder. */
    MirOperand a = mir_value_operand(fn, g, ctx, map, &in->lhs);
    MirOperand b = mir_value_operand(fn, g, ctx, map, &in->rhs);
    return mir_emit1(fn, MIR_CMPBR, mir_op_label(in->text), a, b, 8, 0,
                     0x84 /* je */);
  }

  case IR_OP_ASSIGN: {
    /* Whole-struct copy `@a <- @b` / `@a <- %t` / `%t <- @a`: both operands hold
     * an INDIRECT struct in a LEA-able home, so copy the bytes (rep movsb via the
     * struct-copy helper) instead of an 8-byte MOV that would truncate. */
    {
      const IRFunction *airf =
          ctx && ctx->function_name
              ? code_generator_find_ir_function_binary(g, ctx->function_name)
              : NULL;
      int ssz = mir_operand_struct_home_size(g, airf, &in->dest);
      if (ssz > 0) {
        MirOperand dsym = mir_value_operand(fn, g, ctx, map, &in->dest);
        MirOperand ssym = mir_value_operand(fn, g, ctx, map, &in->lhs);
        if (dsym.kind != MIR_OPK_VREG || ssym.kind != MIR_OPK_VREG) {
          fn->has_error = 1;
          return 0;
        }
        fn->vregs[dsym.vreg].address_taken = 1;
        if (fn->vregs[dsym.vreg].home_bytes < ssz) {
          fn->vregs[dsym.vreg].home_bytes = ssz;
        }
        fn->vregs[ssym.vreg].address_taken = 1;
        if (fn->vregs[ssym.vreg].home_bytes < ssz) {
          fn->vregs[ssym.vreg].home_bytes = ssz;
        }
        MirVregId db = mir_new_vreg(fn, MIR_RC_GP, 8);
        MirVregId sb = mir_new_vreg(fn, MIR_RC_GP, 8);
        if (db == MIR_VREG_NONE || sb == MIR_VREG_NONE ||
            !mir_emit1(fn, MIR_LEA_LOCAL, mir_op_vreg(db), dsym, mir_op_none(), 8,
                       0, 0) ||
            !mir_emit1(fn, MIR_LEA_LOCAL, mir_op_vreg(sb), ssym, mir_op_none(), 8,
                       0, 0) ||
            !mir_emit_struct_copy(fn, db, sb, ssz)) {
          return 0;
        }
        return 1;
      }
    }
    MirOperand dst = mir_value_operand(fn, g, ctx, map, &in->dest);
    int dfb = code_generator_binary_operand_float_bits(g, ctx, &in->dest);
    if (dfb) {
      int sfb = code_generator_binary_operand_float_bits(g, ctx, &in->lhs);
      if (in->lhs.kind == IR_OPERAND_FLOAT) {
        /* Literal at the destination width (pooled if loop-invariant). */
        MirOperand lit = mir_float_const_operand(fn, in->lhs.float_value, dfb / 8);
        return mir_emit_fmov(fn, dst, lit, dfb / 8);
      }
      MirOperand src = mir_value_operand(fn, g, ctx, map, &in->lhs);
      if (sfb && sfb != dfb) {
        /* Float store of a differently-sized value narrows/widens. */
        return mir_emit1(fn, MIR_CVTF2F, dst, src, mir_op_none(), dfb / 8, 0, 0);
      }
      return mir_emit_fmov(fn, dst, src, dfb / 8);
    }
    MirOperand src = mir_value_operand(fn, g, ctx, map, &in->lhs);
    return mir_emit1(fn, MIR_MOV, dst, src, mir_op_none(), 8, 0, 0);
  }

  case IR_OP_BINARY: {
    MirOperand dst = mir_value_operand(fn, g, ctx, map, &in->dest);
    MirOperand a = mir_value_operand(fn, g, ctx, map, &in->lhs);
    MirOperand b = mir_value_operand(fn, g, ctx, map, &in->rhs);
    if (in->is_float) {
      MirOpcode fop = MIR_FADD;
      if (mir_float_arith_opcode(in->text, &fop)) {
        int fb = code_generator_binary_instruction_result_float_bits(g, ctx, in);
        int w = fb ? fb / 8 : 8;
        /* Coerce each operand to the operation width (implicit promotion). */
        MirOperand fa = coerce_float_operand(fn, g, ctx, map, &in->lhs, w);
        MirOperand fbop = coerce_float_operand(fn, g, ctx, map, &in->rhs, w);
        return mir_emit1(fn, fop, dst, fa, fbop, w, 0, 0);
      }
      /* Non-fused ordered float comparison -> 0/1 via ucomis + setcc. */
      int swap;
      unsigned char cc = 0;
      if (!mir_float_cmp_info(in->text, 0, &swap, &cc)) {
        fn->has_error = 1;
        return 0;
      }
      int w = mir_float_cmp_width(g, ctx, in);
      const IROperand *lo = swap ? &in->rhs : &in->lhs;
      const IROperand *ro = swap ? &in->lhs : &in->rhs;
      MirOperand fa = coerce_float_operand(fn, g, ctx, map, lo, w);
      MirOperand fbop = coerce_float_operand(fn, g, ctx, map, ro, w);
      return mir_emit1(fn, MIR_FSETCC, dst, fa, fbop, w, 0, cc);
    }
    if (mir_is_comparison(in->text)) {
      int uns = mir_operand_is_unsigned(g, ctx, &in->lhs) ||
                mir_operand_is_unsigned(g, ctx, &in->rhs);
      unsigned char cc = 0;
      mir_setcc_opcode(in->text, uns, &cc);
      int w = mir_int_compare_width(g, ctx, in->text, &in->lhs, &in->rhs);
      return mir_emit1(fn, MIR_SETCC, dst, a, b, w, uns, cc);
    }
    if (strcmp(in->text, "/") == 0 || strcmp(in->text, "%") == 0) {
      /* idiv/div: signedness is the dividend's (lhs) type; cc carries the
       * quotient-vs-remainder choice (1 == remainder, the `%` case). */
      int uns = mir_operand_is_unsigned(g, ctx, &in->lhs);
      unsigned char mod = (in->text[0] == '%') ? 1 : 0;

      /* Constant-divisor strength reduction: replace the long-latency divide
       * with a magic-number multiply + shifts. Falls through to a real divide
       * for C == 0 (preserves the /0 trap) or unhandled forms. */
      if (in->rhs.kind == IR_OPERAND_INT &&
          mir_emit_const_divmod(fn, dst, a, in->rhs.int_value, uns, mod)) {
        return 1;
      }

      /* Divmod fusion. If a sibling `x op d` already did the divide and captured
       * BOTH results, this op is just a move of the value it needs. */
      if (in->dest.name) {
        for (size_t k = 0; k < fn->divmod_precomp_count; k++) {
          if (fn->divmod_precomp[k].name &&
              strcmp(fn->divmod_precomp[k].name, in->dest.name) == 0) {
            return mir_emit1(fn, MIR_MOV, dst,
                             mir_op_vreg(fn->divmod_precomp[k].vreg),
                             mir_op_none(), 8, 0, 0);
          }
        }
      }

      /* Otherwise look ahead in this basic block for the complementary op (`/`
       * paired with `%`, same operands) so a single divide serves both. The
       * scan stops at a block boundary / call (clobbers RAX:RDX) or any
       * redefinition of the dividend or divisor (would make the cached results
       * stale). */
      const IRFunction *irf =
          ctx && ctx->function_name
              ? code_generator_find_ir_function_binary(g, ctx->function_name)
              : NULL;
      const IRInstruction *sibling = NULL;
      if (irf && in >= irf->instructions &&
          in < irf->instructions + irf->instruction_count &&
          fn->divmod_precomp_count < 16 && in->dest.kind == IR_OPERAND_TEMP) {
        size_t idx = (size_t)(in - irf->instructions);
        for (size_t j = idx + 1; j < irf->instruction_count; j++) {
          const IRInstruction *nx = &irf->instructions[j];
          if (nx->op == IR_OP_LABEL || nx->op == IR_OP_JUMP ||
              nx->op == IR_OP_BRANCH_ZERO || nx->op == IR_OP_BRANCH_EQ ||
              nx->op == IR_OP_CALL || nx->op == IR_OP_RETURN) {
            break;
          }
          if (mir_ir_operand_equal(&nx->dest, &in->lhs) ||
              mir_ir_operand_equal(&nx->dest, &in->rhs)) {
            break;
          }
          if (nx->op == IR_OP_BINARY && nx->text && !nx->is_float &&
              nx->dest.kind == IR_OPERAND_TEMP && nx->dest.name &&
              ((mod && strcmp(nx->text, "/") == 0) ||
               (!mod && strcmp(nx->text, "%") == 0)) &&
              mir_ir_operand_equal(&nx->lhs, &in->lhs) &&
              mir_ir_operand_equal(&nx->rhs, &in->rhs)) {
            sibling = nx;
            break;
          }
        }
      }

      if (sibling) {
        /* One divide; capture quotient (RAX) into qv and remainder (RDX) into
         * rv. The MOV reading RDX must immediately follow the divide (nothing
         * between can clobber RDX, which is non-allocatable scratch). */
        MirVregId qv = mir_new_vreg(fn, MIR_RC_GP, 8);
        MirVregId rv = mir_new_vreg(fn, MIR_RC_GP, 8);
        if (qv == MIR_VREG_NONE || rv == MIR_VREG_NONE) {
          return 0;
        }
        if (!mir_emit1(fn, MIR_IDIV, mir_op_vreg(qv), a, b, 8, uns, 0) ||
            !mir_emit1(fn, MIR_MOV, mir_op_vreg(rv),
                       mir_op_phys(BINARY_GP_RDX, MIR_RC_GP), mir_op_none(), 8, 0,
                       0)) {
          return 0;
        }
        MirVregId mine = mod ? rv : qv;     /* this op's result */
        MirVregId theirs = mod ? qv : rv;   /* the sibling's result */
        fn->divmod_precomp[fn->divmod_precomp_count].name = sibling->dest.name;
        fn->divmod_precomp[fn->divmod_precomp_count].vreg = theirs;
        fn->divmod_precomp_count++;
        return mir_emit1(fn, MIR_MOV, dst, mir_op_vreg(mine), mir_op_none(), 8, 0,
                         0);
      }

      return mir_emit1(fn, MIR_IDIV, dst, a, b, 8, uns, mod);
    }
    MirOpcode op = MIR_ADD;
    mir_arith_opcode(in->text, &op);
    int uns = 0;
    if (op == MIR_SHR) {
      /* arithmetic vs logical right shift depends on the LHS signedness. */
      if (!mir_operand_is_unsigned(g, ctx, &in->lhs)) {
        op = MIR_SAR;
      } else {
        uns = 1;
      }
    }
    return mir_emit1(fn, op, dst, a, b, 8, uns, 0);
  }

  case IR_OP_UNARY: {
    MirOperand dst = mir_value_operand(fn, g, ctx, map, &in->dest);
    const char *op = in->text ? in->text : "";
    if (in->is_float) {
      /* Float negate `-x` as `0 - x` (pxor zero; subss/subsd), matching the
       * fallback's emit_unary exactly so 0/NaN signs agree; `+x` is a copy. The
       * operand is coerced to the result precision first. */
      int fb = code_generator_binary_instruction_result_float_bits(g, ctx, in);
      int w = fb ? fb / 8 : 8;
      MirOperand x = coerce_float_operand(fn, g, ctx, map, &in->lhs, w);
      if (strcmp(op, "+") == 0) {
        return mir_emit_fmov(fn, dst, x, w);
      }
      MirOperand zero = mir_float_const_operand(fn, 0.0, w);
      return mir_emit1(fn, MIR_FSUB, dst, zero, x, w, 0, 0);
    }
    MirOperand a = mir_value_operand(fn, g, ctx, map, &in->lhs);
    if (strcmp(op, "-") == 0) {
      return mir_emit1(fn, MIR_NEG, dst, a, mir_op_none(), 8, 0, 0);
    }
    if (strcmp(op, "~") == 0) {
      return mir_emit1(fn, MIR_NOT, dst, a, mir_op_none(), 8, 0, 0);
    }
    if (strcmp(op, "+") == 0) {
      return mir_emit1(fn, MIR_MOV, dst, a, mir_op_none(), 8, 0, 0);
    }
    if (strcmp(op, "!") == 0) {
      /* !x == (x == 0) as 0/1: SETCC does cmp a,0; sete; movzx. */
      unsigned char cc = 0;
      mir_setcc_opcode("==", 0, &cc);
      return mir_emit1(fn, MIR_SETCC, dst, a, mir_op_imm(0), 8, 0, cc);
    }
    fn->has_error = 1;
    return 0;
  }

  case IR_OP_CAST: {
    MirOperand dst = mir_value_operand(fn, g, ctx, map, &in->dest);
    MirOperand a = mir_value_operand(fn, g, ctx, map, &in->lhs);
    int dfb = code_generator_binary_operand_float_bits(g, ctx, &in->dest);
    int sfb = code_generator_binary_operand_float_bits(g, ctx, &in->lhs);
    if (dfb && !sfb) {
      /* int -> float */
      return mir_emit1(fn, MIR_CVTSI2F, dst, a, mir_op_none(), dfb / 8, 0, 0);
    }
    if (!dfb && sfb) {
      /* float -> int (truncating); width selects cvttsd2si vs cvttss2si. */
      return mir_emit1(fn, MIR_CVTF2SI, dst, a, mir_op_none(), sfb / 8, 0, 0);
    }
    if (dfb && sfb) {
      /* float -> float; same width is just a copy, else cvtsd2ss/cvtss2sd. */
      if (dfb == sfb) {
        return mir_emit_fmov(fn, dst, a, dfb / 8);
      }
      return mir_emit1(fn, MIR_CVTF2F, dst, a, mir_op_none(), dfb / 8, 0, 0);
    }
    /* The cast's target type is named on the instruction (in->text) and is
     * always resolvable; the dest operand's type is not (a temp has no
     * resolved type at -O0, which would silently drop a narrowing cast). Prefer
     * in->text, matching the fallback emitter, and fall back to the operand. */
    Type *dt = (in->text && g->type_checker)
                   ? type_checker_get_type_by_name(g->type_checker, in->text)
                   : NULL;
    if (!dt) {
      dt = code_generator_binary_get_operand_type_in_context(g, ctx, &in->dest);
    }
    int dw = dt ? code_generator_binary_resolved_type_scalar_size(dt) : 8;
    int dsigned = dt ? code_generator_binary_resolved_type_is_signed_integer(dt)
                     : 1;
    if (dw != 1 && dw != 2 && dw != 4 && dw != 8) {
      dw = 8;
    }
    /* Re-express a's 64-bit value as the dst integer type. A NARROWING cast
     * (dw < source width) truncates to dw bytes then extends per dst signedness.
     * A WIDENING cast (dw >= source width) must extend from the SOURCE width per
     * the SOURCE signedness, because MIR computes in 64-bit and a narrow source
     * value (e.g. a uint32 product) can carry garbage above its width — a plain
     * 64-bit move would carry that garbage into the wider value (e.g.
     * `(int64)(uint32_a * uint32_b)`). */
    Type *st = code_generator_binary_get_operand_type_in_context(g, ctx, &in->lhs);
    int sw = st ? code_generator_binary_resolved_type_scalar_size(st) : 0;
    int ssigned = st ? code_generator_binary_resolved_type_is_signed_integer(st)
                     : 1;
    int swf = st ? code_generator_binary_resolved_type_float_bits(st) : 0;
    if ((sw == 1 || sw == 2 || sw == 4) && swf == 0 && dw >= sw) {
      /* Widening (or same-width) from a known narrow integer source: canonicalize
       * by extending from the source width per the source signedness. */
      return mir_emit1(fn, ssigned ? MIR_MOVSX : MIR_MOVZX, dst, a,
                       mir_op_none(), sw, !ssigned, 0);
    }
    if (dw == 8) {
      /* Widening to 64 bits from an 8-byte or unknown source: a plain move. */
      return mir_emit1(fn, MIR_MOV, dst, a, mir_op_none(), 8, 0, 0);
    }
    /* Narrowing to a < source-width dst: truncate+extend per dst signedness. */
    return mir_emit1(fn, dsigned ? MIR_MOVSX : MIR_MOVZX, dst, a, mir_op_none(),
                     dw, !dsigned, 0);
  }

  case IR_OP_LOAD: {
    if (in->lhs.kind == IR_OPERAND_STRING) {
      /* Data-pointer field of a string literal: materialize the .rdata
       * cstring address directly (validated to be the 8-byte pointer load by
       * the eligibility gate). */
      MirOperand dst = mir_value_operand(fn, g, ctx, map, &in->dest);
      const char *s = in->lhs.name ? in->lhs.name : "";
      return mir_emit1(fn, MIR_LEA_CSTR, dst, mir_op_symbol(s), mir_op_none(),
                       8, 0, 0);
    }
    MirOperand dst = mir_value_operand(fn, g, ctx, map, &in->dest);
    MirOperand addr = mir_value_operand(fn, g, ctx, map, &in->lhs);
    int size = code_generator_binary_get_access_size(g, ctx, &in->rhs);
    if (size <= 0) {
      fn->has_error = 1;
      return 0;
    }
    MirOperand mem = mir_op_mem_vreg(addr.vreg, MIR_VREG_NONE, 1, 0);
    if (in->is_float) {
      int fb = code_generator_binary_instruction_result_float_bits(g, ctx, in);
      return mir_emit_fmov(fn, dst, mem, fb ? fb / 8 : size);
    }
    int sign_ext = !in->is_unsigned &&
                   code_generator_binary_load_needs_sign_extend(g, ctx,
                                                               &in->dest, size);
    return mir_emit1(fn, MIR_MOV, dst, mem, mir_op_none(), size,
                     sign_ext ? 0 : 1, 0);
  }

  case IR_OP_STORE: {
    MirOperand addr = mir_value_operand(fn, g, ctx, map, &in->dest);
    int size = code_generator_binary_get_access_size(g, ctx, &in->rhs);
    if (size <= 0) {
      fn->has_error = 1;
      return 0;
    }
    if (size != 1 && size != 2 && size != 4 && size != 8) {
      MirOperand src_base = mir_value_operand(fn, g, ctx, map, &in->lhs);
      if (addr.kind != MIR_OPK_VREG || src_base.kind != MIR_OPK_VREG) {
        fn->has_error = 1;
        return 0;
      }
      return mir_emit_struct_copy(fn, addr.vreg, src_base.vreg, size);
    }
    MirOperand mem = mir_op_mem_vreg(addr.vreg, MIR_VREG_NONE, 1, 0);
    if (in->is_float) {
      /* Coerce the value to the store width: a literal is materialized at
       * that width, and a float64-tracked arithmetic result narrows via
       * cvtsd2ss before a 4-byte store (a raw movss of a double's low dword
       * silently stores garbage — 0 for round values). */
      MirOperand fval =
          coerce_float_operand(fn, g, ctx, map, &in->lhs, size);
      return mir_emit_fmov(fn, mem, fval, size);
    }
    MirOperand val = mir_value_operand(fn, g, ctx, map, &in->lhs);
    return mir_emit1(fn, MIR_MOV, mem, val, mir_op_none(), size, 0, 0);
  }

  case IR_OP_PREFETCH: {
    MirOperand addr = mir_value_operand(fn, g, ctx, map, &in->lhs);
    if (addr.kind != MIR_OPK_VREG) {
      fn->has_error = 1;
      return 0;
    }
    MirOperand mem = mir_op_mem_vreg(addr.vreg, MIR_VREG_NONE, 1, 0);
    return mir_emit1(fn, MIR_PREFETCH, mir_op_none(), mem, mir_op_none(), 8, 0,
                     0);
  }

  case IR_OP_SELECT: {
    /* dst = (cond != 0) ? then : else. Stage cond and then in vregs, pre-load
     * a result vreg with else, then MIR_CMOV res, cond, then. Pre-loading res
     * makes its live range start before the cmov so it interferes with
     * cond/then and gets a distinct register (cmov needs res != then). Finally
     * move res into the IR dest (which may be a memory-resident local). */
    MirOperand cond = mir_value_operand(fn, g, ctx, map, &in->lhs);
    MirOperand then_v = mir_value_operand(fn, g, ctx, map, &in->rhs);
    MirOperand else_v = mir_value_operand(fn, g, ctx, map, &in->arguments[0]);
    MirOperand dest = mir_value_operand(fn, g, ctx, map, &in->dest);
    MirVregId cond_r = mir_new_vreg(fn, MIR_RC_GP, 8);
    MirVregId then_r = mir_new_vreg(fn, MIR_RC_GP, 8);
    MirVregId res_r = mir_new_vreg(fn, MIR_RC_GP, 8);
    if (cond_r == MIR_VREG_NONE || then_r == MIR_VREG_NONE ||
        res_r == MIR_VREG_NONE) {
      return 0;
    }
    if (!mir_emit1(fn, MIR_MOV, mir_op_vreg(cond_r), cond, mir_op_none(), 8, 0,
                   0) ||
        !mir_emit1(fn, MIR_MOV, mir_op_vreg(then_r), then_v, mir_op_none(), 8,
                   0, 0) ||
        !mir_emit1(fn, MIR_MOV, mir_op_vreg(res_r), else_v, mir_op_none(), 8, 0,
                   0) ||
        !mir_emit1(fn, MIR_CMOV, mir_op_vreg(res_r), mir_op_vreg(cond_r),
                   mir_op_vreg(then_r), 8, 0, 0)) {
      return 0;
    }
    return mir_emit1(fn, MIR_MOV, dest, mir_op_vreg(res_r), mir_op_none(), 8, 0,
                     0);
  }

  case IR_OP_RETURN: {
    if (fn->returns_indirect && in->lhs.kind == IR_OPERAND_SYMBOL) {
      /* INDIRECT struct return: copy the struct local into the caller's hidden
       * slot (whose pointer the prologue homed into indirect_return_vreg), then
       * leave that pointer in RAX as the Win64/SysV ABI requires. The source is
       * the struct local's stack home; LEA it like any &@local. */
      MirOperand structv = mir_value_operand(fn, g, ctx, map, &in->lhs);
      if (structv.kind != MIR_OPK_VREG ||
          fn->indirect_return_vreg == MIR_VREG_NONE) {
        fn->has_error = 1;
        return 0;
      }
      fn->vregs[structv.vreg].address_taken = 1;
      if (fn->vregs[structv.vreg].home_bytes < fn->indirect_return_size) {
        fn->vregs[structv.vreg].home_bytes =
            (fn->indirect_return_size + 7) & ~7;
      }
      MirVregId src_base = mir_new_vreg(fn, MIR_RC_GP, 8);
      if (src_base == MIR_VREG_NONE ||
          !mir_emit1(fn, MIR_LEA_LOCAL, mir_op_vreg(src_base), structv,
                     mir_op_none(), 8, 0, 0) ||
          !mir_emit_struct_copy(fn, fn->indirect_return_vreg, src_base,
                                fn->indirect_return_size)) {
        return 0;
      }
      /* Writeback before the RAX move (the flush uses RAX as scratch). */
      if (!mir_emit_global_writebacks(fn, g, map, wb)) {
        return 0;
      }
      if (!mir_emit1(fn, MIR_MOV, mir_op_phys(BINARY_GP_RAX, MIR_RC_GP),
                     mir_op_vreg(fn->indirect_return_vreg), mir_op_none(), 8, 0,
                     0)) {
        return 0;
      }
      return mir_emit1(fn, MIR_RET, mir_op_none(), mir_op_none(), mir_op_none(),
                       8, 0, 0);
    }
    /* Flush register-promoted globals to memory BEFORE materializing the return
     * value. The writeback uses RAX as scratch to store spilled (memory-homed)
     * global caches, so doing it after the return value is placed in RAX would
     * clobber that value (e.g. `return some_global` corrupted by a later cache
     * flush). The writeback only reads cache vregs and stores to memory, so the
     * return source vreg is still valid afterwards. */
    if (!mir_emit_global_writebacks(fn, g, map, wb)) {
      return 0;
    }
    if (in->lhs.kind != IR_OPERAND_NONE) {
      MirOperand src = mir_value_operand(fn, g, ctx, map, &in->lhs);
      int rfb = code_generator_binary_operand_float_bits(g, ctx, &in->lhs);
      if (rfb) {
        /* Float return value goes in XMM0, converted to the DECLARED return
         * width: a float64-tracked temp returned from a float32 function
         * narrows via cvtsd2ss (a raw movss would hand the caller the low
         * dword of a double). */
        int want = fn->float_return_bits ? fn->float_return_bits : rfb;
        if (want != rfb) {
          MirVregId tmp = mir_new_vreg(fn, MIR_RC_XMM, want / 8);
          if (tmp == MIR_VREG_NONE ||
              !mir_emit1(fn, MIR_CVTF2F, mir_op_vreg(tmp), src, mir_op_none(),
                         want / 8, 0, 0)) {
            return 0;
          }
          src = mir_op_vreg(tmp);
        }
        if (!mir_emit_fmov(fn, mir_op_phys(BINARY_XMM0, MIR_RC_XMM), src,
                           want / 8)) {
          return 0;
        }
      } else if (fn->scalar_return_width == 1 || fn->scalar_return_width == 2 ||
                 fn->scalar_return_width == 4) {
        /* Canonicalize a narrow integer return to 64 bits (the high RAX bits
         * are ABI-undefined for a sub-64-bit return, and MIR may have left
         * garbage there) so a caller using the full register is correct. */
        if (!mir_emit1(fn,
                       fn->scalar_return_signed ? MIR_MOVSX : MIR_MOVZX,
                       mir_op_phys(BINARY_GP_RAX, MIR_RC_GP), src, mir_op_none(),
                       fn->scalar_return_width, !fn->scalar_return_signed, 0)) {
          return 0;
        }
      } else if (!mir_emit1(fn, MIR_MOV, mir_op_phys(BINARY_GP_RAX, MIR_RC_GP),
                            src, mir_op_none(), 8, 0, 0)) {
        return 0;
      }
    }
    return mir_emit1(fn, MIR_RET, mir_op_none(), mir_op_none(), mir_op_none(), 8,
                     0, 0);
  }

  case IR_OP_CALL: {
    /* A failed-safety-check trap: lower to a terminal MIR_TRAP carrying the
     * abort message (the STRING argument). MIR only runs without stack-trace
     * support, so the trap degrades to puts(message)+exit(1); the remaining
     * trap arguments (kind, pc, rbp) are unused on that path. */
    if (mir_call_is_runtime_trap(in)) {
      int msg_idx = strcmp(in->text, "mettle_crash_trap_ex") == 0 ? 1 : 0;
      const char *msg = "";
      if ((size_t)msg_idx < in->argument_count &&
          in->arguments[msg_idx].kind == IR_OPERAND_STRING &&
          in->arguments[msg_idx].name) {
        msg = in->arguments[msg_idx].name;
      }
      /* The abort message goes in operand `a` (MIR_TRAP reads in->a.sym). */
      return mir_emit1(fn, MIR_TRAP, mir_op_none(), mir_op_symbol(msg),
                       mir_op_none(), 8, 0, 0);
    }
    /* Declare external callees so the linker resolves the relocation. */
    IRFunction *target = code_generator_find_ir_function_binary(g, in->text);
    if (!target) {
      const char *link = code_generator_get_link_symbol_name(g, in->text);
      if (link && !code_generator_binary_declare_external_symbol(g, link)) {
        fn->has_error = 1;
        return 0;
      }
    }
    /* Marshal arguments per the ABI layout. Arguments up to the ABI's
     * argument-register count go into registers (GP, or XMM for float args); the
     * rest are stored into the outgoing stack-argument region (reserved once in
     * the prologue). Eligibility guarantees every float arg lands in an XMM
     * register (float stack args still bail). */
    const BinaryAbi *abi = code_generator_binary_active_abi();
    /* Caller-side INDIRECT return: the callee returns a struct by value, so the
     * ABI passes a hidden out-pointer as the first integer arg, shifting every
     * user arg up one slot. We point the hidden arg at the destination struct
     * LOCAL's home so the callee writes the result there directly. */
    int ret_indirect = 0;
    {
      Symbol *rc =
          g->symbol_table ? symbol_table_lookup(g->symbol_table, in->text) : NULL;
      Type *rret = (rc && rc->kind == SYMBOL_FUNCTION)
                       ? (rc->data.function.return_type ? rc->data.function.return_type
                                                        : rc->type)
                       : NULL;
      if (rret && code_generator_abi_classify(rret) == ABI_PASS_INDIRECT &&
          (in->dest.kind == IR_OPERAND_SYMBOL ||
           in->dest.kind == IR_OPERAND_TEMP)) {
        ret_indirect = 1;
      }
    }
    int hidden = ret_indirect ? 1 : 0;
    int arg_is_float[MIR_MAX_PARAMS + 1] = {0}; /* slot 0 = hidden ptr if present */
    /* Tag each positional slot's float class from the callee's parameter types so
     * the ABI layout routes float args to XMM registers. Without this every arg
     * defaults to integer and a float arg is homed into a GP register (and a
     * float immediate then reaches the GP value path — an encoder error). */
    {
      Symbol *fc = g->symbol_table
                       ? symbol_table_lookup(g->symbol_table, in->text)
                       : NULL;
      if (fc && fc->kind == SYMBOL_FUNCTION &&
          fc->data.function.parameter_types) {
        for (size_t a = 0; a < in->argument_count &&
                           a < fc->data.function.parameter_count;
             a++) {
          Type *pt = fc->data.function.parameter_types[a];
          if (pt && code_generator_binary_resolved_type_float_bits(pt) != 0) {
            arg_is_float[a + (size_t)hidden] = 1;
          }
        }
      }
    }
    BinaryArgLocation locs[MIR_MAX_PARAMS + 1];
    int stack_bytes = 0;
    size_t nlocs = in->argument_count + (size_t)hidden;
    if (nlocs > (size_t)(MIR_MAX_PARAMS + 1)) {
      fn->has_error = 1;
      return 0;
    }
    if (nlocs > 0 &&
        !code_generator_binary_compute_arg_layout(abi, arg_is_float, nlocs, locs,
                                                  &stack_bytes)) {
      fn->has_error = 1;
      return 0;
    }
    if (stack_bytes > fn->outgoing_stack_bytes) {
      fn->outgoing_stack_bytes = stack_bytes;
    }
    /* INDIRECT (by-value) struct arguments: the ABI passes a pointer to a
     * caller-made copy. Lay out a copy slot per such arg in the outgoing
     * indirect region (at the bottom of the frame), copy each struct there, and
     * pass &slot as the (integer) argument value. Eligibility has proven every
     * INDIRECT arg is a struct LOCAL, so its source is its stack home. */
    int indirect_off[MIR_MAX_PARAMS] = {0}; /* slot offset, or -1 if not indirect */
    Symbol *call_callee =
        g->symbol_table ? symbol_table_lookup(g->symbol_table, in->text) : NULL;
    int indirect_region = 0;
    for (size_t a = 0; a < in->argument_count; a++) {
      indirect_off[a] = -1;
      Type *pt = (call_callee && call_callee->kind == SYMBOL_FUNCTION &&
                  call_callee->data.function.parameter_types)
                     ? call_callee->data.function.parameter_types[a]
                     : NULL;
      if (!pt || code_generator_abi_classify(pt) != ABI_PASS_INDIRECT) {
        continue;
      }
      int sz = (int)code_generator_abi_type_size(pt);
      indirect_off[a] = indirect_region;
      indirect_region += (sz + 7) & ~7;
      /* Copy the struct from its local home into the slot. */
      MirOperand structv = mir_value_operand(fn, g, ctx, map, &in->arguments[a]);
      if (structv.kind != MIR_OPK_VREG) {
        fn->has_error = 1;
        return 0;
      }
      fn->vregs[structv.vreg].address_taken = 1;
      if (fn->vregs[structv.vreg].home_bytes < ((sz + 7) & ~7)) {
        fn->vregs[structv.vreg].home_bytes = (sz + 7) & ~7;
      }
      MirVregId src_base = mir_new_vreg(fn, MIR_RC_GP, 8);
      MirVregId dst_base = mir_new_vreg(fn, MIR_RC_GP, 8);
      if (src_base == MIR_VREG_NONE || dst_base == MIR_VREG_NONE ||
          !mir_emit1(fn, MIR_LEA_LOCAL, mir_op_vreg(src_base), structv,
                     mir_op_none(), 8, 0, 0) ||
          !mir_emit1(fn, MIR_LEA_OUTARG, mir_op_vreg(dst_base),
                     mir_op_imm(indirect_off[a]), mir_op_none(), 8, 0, 0) ||
          !mir_emit_struct_copy(fn, dst_base, src_base, sz)) {
        return 0;
      }
    }
    if (indirect_region > 0) {
      indirect_region = (indirect_region + 15) & ~15;
      if (indirect_region > fn->outgoing_indirect_bytes) {
        fn->outgoing_indirect_bytes = indirect_region;
      }
    }
    /* Stack args first: they read their source vregs before any argument
     * register is written, so a reg-move below can never clobber a stack arg's
     * source. The slot is above the shadow space at a fixed rsp offset. */
    for (size_t a = 0; a < in->argument_count; a++) {
      if (locs[a + hidden].kind != BINARY_ARG_ON_STACK) {
        continue;
      }
      int slot = abi->shadow_space_size + locs[a + hidden].stack_offset;
      MirOperand val;
      if (indirect_off[a] >= 0) {
        /* INDIRECT struct arg: pass &copy_slot. */
        MirVregId t = mir_new_vreg(fn, MIR_RC_GP, 8);
        if (t == MIR_VREG_NONE ||
            !mir_emit1(fn, MIR_LEA_OUTARG, mir_op_vreg(t),
                       mir_op_imm(indirect_off[a]), mir_op_none(), 8, 0, 0)) {
          return 0;
        }
        val = mir_op_vreg(t);
      } else if (in->arguments[a].kind == IR_OPERAND_STRING) {
        /* Stage the cstring address in a temp, then store it to the slot. */
        const char *s = in->arguments[a].name ? in->arguments[a].name : "";
        MirVregId t = mir_new_vreg(fn, MIR_RC_GP, 8);
        if (t == MIR_VREG_NONE ||
            !mir_emit1(fn, MIR_LEA_CSTR, mir_op_vreg(t), mir_op_symbol(s),
                       mir_op_none(), 8, 0, 0)) {
          return 0;
        }
        val = mir_op_vreg(t);
      } else {
        val = mir_value_operand(fn, g, ctx, map, &in->arguments[a]);
      }
      if (!mir_emit1(fn, MIR_STORE_OUTARG, mir_op_none(), val,
                     mir_op_imm(slot), 8, 0, 0)) {
        return 0;
      }
    }
    /* Register args. The target registers are never allocatable, so these moves
     * cannot clobber one another's sources. */
    for (size_t a = 0; a < in->argument_count; a++) {
      if (locs[a + hidden].kind != BINARY_ARG_IN_GP_REGISTER) {
        continue;
      }
      BinaryGpRegister reg = locs[a + hidden].gp_register;
      if (indirect_off[a] >= 0) {
        /* INDIRECT struct arg: lea &copy_slot directly into the ABI arg reg. */
        if (!mir_emit1(fn, MIR_LEA_OUTARG, mir_op_phys(reg, MIR_RC_GP),
                       mir_op_imm(indirect_off[a]), mir_op_none(), 8, 0, 0)) {
          return 0;
        }
        continue;
      }
      if (in->arguments[a].kind == IR_OPERAND_STRING) {
        /* A string-literal argument is passed as the address of its .rdata
         * cstring (lea directly into the ABI argument register). */
        const char *s = in->arguments[a].name ? in->arguments[a].name : "";
        if (!mir_emit1(fn, MIR_LEA_CSTR, mir_op_phys(reg, MIR_RC_GP),
                       mir_op_symbol(s), mir_op_none(), 8, 0, 0)) {
          return 0;
        }
        continue;
      }
      MirOperand arg = mir_value_operand(fn, g, ctx, map, &in->arguments[a]);
      if (!mir_emit1(fn, MIR_MOV, mir_op_phys(reg, MIR_RC_GP), arg,
                     mir_op_none(), 8, 0, 0)) {
        return 0;
      }
    }
    /* Float register args: move each value into its XMM argument register,
     * converting to the parameter's precision (a float64-tracked temp passed to
     * a float32 param narrows via cvtsd2ss — mirroring the fallback's
     * emit_float_call_argument; a raw movss would hand the callee the low dword
     * of a double, zero for values like 2.0). Setting has_xmm_arg_call removes
     * XMM0..XMM3 from this function's allocation pool, so no arg source ever sits
     * in a target register and these moves cannot clobber a not-yet-consumed
     * source (the parallel-move hazard with 2+ float args). */
    for (size_t a = 0; a < in->argument_count; a++) {
      if (locs[a + hidden].kind != BINARY_ARG_IN_XMM_REGISTER) {
        continue;
      }
      fn->has_xmm_arg_call = 1;
      BinaryXmmRegister xreg = locs[a + hidden].xmm_register;
      Type *pt = (call_callee && call_callee->kind == SYMBOL_FUNCTION &&
                  call_callee->data.function.parameter_types)
                     ? call_callee->data.function.parameter_types[a]
                     : NULL;
      int pfb = pt ? code_generator_binary_resolved_type_float_bits(pt) : 0;
      if (pfb != 32 && pfb != 64) {
        pfb = 64;
      }
      const IROperand *arg_op = &in->arguments[a];
      int sfb;
      if (arg_op->kind == IR_OPERAND_FLOAT) {
        sfb = arg_op->float_bits == 32 ? 32 : 64;
      } else {
        sfb = code_generator_binary_operand_float_bits(g, ctx, arg_op);
        if (sfb != 32 && sfb != 64) {
          sfb = pfb;
        }
      }
      MirOperand val = mir_value_operand(fn, g, ctx, map, arg_op);
      if (val.kind == MIR_OPK_FIMM) {
        /* A float immediate cannot move straight into a physical register; stage
         * it (at its own precision) in a vreg first. */
        MirVregId t = mir_new_vreg(fn, MIR_RC_XMM, sfb / 8);
        if (t == MIR_VREG_NONE ||
            !mir_emit_fmov(fn, mir_op_vreg(t), val, sfb / 8)) {
          return 0;
        }
        val = mir_op_vreg(t);
      }
      if (sfb != pfb) {
        MirVregId t2 = mir_new_vreg(fn, MIR_RC_XMM, pfb / 8);
        if (t2 == MIR_VREG_NONE ||
            !mir_emit1(fn, MIR_CVTF2F, mir_op_vreg(t2), val, mir_op_none(),
                       pfb / 8, 0, 0)) {
          return 0;
        }
        val = mir_op_vreg(t2);
      }
      if (!mir_emit_fmov(fn, mir_op_phys(xreg, MIR_RC_XMM), val, pfb / 8)) {
        return 0;
      }
    }
    /* Hidden INDIRECT-return pointer: lea the destination struct local's home
     * into the ABI's out-pointer register (slot 0). The callee writes the
     * returned struct directly there, so no post-call copy is needed. */
    if (ret_indirect) {
      MirOperand dstsym = mir_value_operand(fn, g, ctx, map, &in->dest);
      if (dstsym.kind != MIR_OPK_VREG) {
        fn->has_error = 1;
        return 0;
      }
      fn->vregs[dstsym.vreg].address_taken = 1;
      /* Size the dest's home to the whole struct (a struct LOCAL or struct TEMP
       * — mir_operand_struct_home_size resolves a temp's size from the IR). */
      {
        const IRFunction *dirf =
            ctx && ctx->function_name
                ? code_generator_find_ir_function_binary(g, ctx->function_name)
                : NULL;
        int hb = mir_operand_struct_home_size(g, dirf, &in->dest);
        if (hb > 0 && fn->vregs[dstsym.vreg].home_bytes < hb) {
          fn->vregs[dstsym.vreg].home_bytes = hb;
        }
      }
      if (!mir_emit1(fn, MIR_LEA_LOCAL,
                     mir_op_phys(abi->indirect_return_register, MIR_RC_GP),
                     dstsym, mir_op_none(), 8, 0, 0)) {
        return 0;
      }
    }
    if (!mir_emit1(fn, MIR_CALL, mir_op_symbol(in->text), mir_op_none(),
                   mir_op_none(), 8, 0, 0)) {
      return 0;
    }
    if (ret_indirect) {
      /* The struct result was written into the dest local's home by the callee;
       * nothing to move out of RAX. */
      return 1;
    }
    /* Move the return value out of RAX / XMM0 before anything clobbers it. */
    if (in->dest.kind == IR_OPERAND_TEMP || in->dest.kind == IR_OPERAND_SYMBOL) {
      int rfb = code_generator_binary_operand_float_bits(g, ctx, &in->dest);
      MirOperand dst = mir_value_operand(fn, g, ctx, map, &in->dest);
      if (rfb) {
        return mir_emit_fmov(fn, dst, mir_op_phys(BINARY_XMM0, MIR_RC_XMM),
                             rfb / 8);
      }
      return mir_emit1(fn, MIR_MOV, dst,
                       mir_op_phys(BINARY_GP_RAX, MIR_RC_GP), mir_op_none(), 8,
                       0, 0);
    }
    return 1;
  }

  case IR_OP_CALL_INDIRECT: {
    const IRFunction *irf =
        ctx && ctx->function_name
            ? code_generator_find_ir_function_binary(g, ctx->function_name)
            : NULL;
    Type *ft = mir_indirect_call_type(g, irf, in);
    if (!ft) {
      fn->has_error = 1;
      return 0;
    }
    const BinaryAbi *abi = code_generator_binary_active_abi();
    int arg_is_float[MIR_MAX_PARAMS] = {0};
    BinaryArgLocation locs[MIR_MAX_PARAMS];
    int stack_bytes = 0;
    for (size_t a = 0; a < in->argument_count; a++) {
      Type *pt = ft->fn_param_types ? ft->fn_param_types[a] : NULL;
      arg_is_float[a] =
          code_generator_binary_resolved_type_float_bits(pt) != 0;
    }
    if (in->argument_count > 0 &&
        !code_generator_binary_compute_arg_layout(abi, arg_is_float,
                                                  in->argument_count, locs,
                                                  &stack_bytes)) {
      fn->has_error = 1;
      return 0;
    }
    if (stack_bytes > fn->outgoing_stack_bytes) {
      fn->outgoing_stack_bytes = stack_bytes;
    }

    MirOperand callee = mir_value_operand(fn, g, ctx, map, &in->lhs);

    for (size_t a = 0; a < in->argument_count; a++) {
      if (locs[a].kind != BINARY_ARG_ON_STACK) {
        continue;
      }
      int slot = abi->shadow_space_size + locs[a].stack_offset;
      MirOperand val;
      if (in->arguments[a].kind == IR_OPERAND_STRING) {
        const char *s = in->arguments[a].name ? in->arguments[a].name : "";
        MirVregId t = mir_new_vreg(fn, MIR_RC_GP, 8);
        if (t == MIR_VREG_NONE ||
            !mir_emit1(fn, MIR_LEA_CSTR, mir_op_vreg(t), mir_op_symbol(s),
                       mir_op_none(), 8, 0, 0)) {
          return 0;
        }
        val = mir_op_vreg(t);
      } else {
        val = mir_value_operand(fn, g, ctx, map, &in->arguments[a]);
      }
      if (!mir_emit1(fn, MIR_STORE_OUTARG, mir_op_none(), val,
                     mir_op_imm(slot), 8, 0, 0)) {
        return 0;
      }
    }

    for (size_t a = 0; a < in->argument_count; a++) {
      if (locs[a].kind != BINARY_ARG_IN_GP_REGISTER) {
        continue;
      }
      BinaryGpRegister reg = locs[a].gp_register;
      if (in->arguments[a].kind == IR_OPERAND_STRING) {
        const char *s = in->arguments[a].name ? in->arguments[a].name : "";
        if (!mir_emit1(fn, MIR_LEA_CSTR, mir_op_phys(reg, MIR_RC_GP),
                       mir_op_symbol(s), mir_op_none(), 8, 0, 0)) {
          return 0;
        }
        continue;
      }
      MirOperand arg = mir_value_operand(fn, g, ctx, map, &in->arguments[a]);
      if (!mir_emit1(fn, MIR_MOV, mir_op_phys(reg, MIR_RC_GP), arg,
                     mir_op_none(), 8, 0, 0)) {
        return 0;
      }
    }

    for (size_t a = 0; a < in->argument_count; a++) {
      if (locs[a].kind != BINARY_ARG_IN_XMM_REGISTER) {
        continue;
      }
      fn->has_xmm_arg_call = 1;
      BinaryXmmRegister xreg = locs[a].xmm_register;
      Type *pt = ft->fn_param_types ? ft->fn_param_types[a] : NULL;
      int pfb = code_generator_binary_resolved_type_float_bits(pt);
      if (pfb != 32 && pfb != 64) {
        pfb = 64;
      }
      const IROperand *arg_op = &in->arguments[a];
      int sfb;
      if (arg_op->kind == IR_OPERAND_FLOAT) {
        sfb = arg_op->float_bits == 32 ? 32 : 64;
      } else {
        sfb = code_generator_binary_operand_float_bits(g, ctx, arg_op);
        if (sfb != 32 && sfb != 64) {
          sfb = pfb;
        }
      }
      MirOperand val = mir_value_operand(fn, g, ctx, map, arg_op);
      if (val.kind == MIR_OPK_FIMM) {
        MirVregId t = mir_new_vreg(fn, MIR_RC_XMM, sfb / 8);
        if (t == MIR_VREG_NONE ||
            !mir_emit_fmov(fn, mir_op_vreg(t), val, sfb / 8)) {
          return 0;
        }
        val = mir_op_vreg(t);
      }
      if (sfb != pfb) {
        MirVregId t2 = mir_new_vreg(fn, MIR_RC_XMM, pfb / 8);
        if (t2 == MIR_VREG_NONE ||
            !mir_emit1(fn, MIR_CVTF2F, mir_op_vreg(t2), val, mir_op_none(),
                       pfb / 8, 0, 0)) {
          return 0;
        }
        val = mir_op_vreg(t2);
      }
      if (!mir_emit_fmov(fn, mir_op_phys(xreg, MIR_RC_XMM), val, pfb / 8)) {
        return 0;
      }
    }

    if (!mir_emit1(fn, MIR_CALL_INDIRECT, mir_op_none(),
                   callee, mir_op_none(), 8, 0, 0)) {
      return 0;
    }

    if (in->dest.kind == IR_OPERAND_TEMP || in->dest.kind == IR_OPERAND_SYMBOL) {
      int rfb = code_generator_binary_resolved_type_float_bits(ft->fn_return_type);
      MirOperand dst = mir_value_operand(fn, g, ctx, map, &in->dest);
      if (rfb) {
        return mir_emit_fmov(fn, dst, mir_op_phys(BINARY_XMM0, MIR_RC_XMM),
                             rfb / 8);
      }
      return mir_emit1(fn, MIR_MOV, dst,
                       mir_op_phys(BINARY_GP_RAX, MIR_RC_GP), mir_op_none(), 8,
                       0, 0);
    }
    return 1;
  }

  case IR_OP_SIMD_SLP_MAC_I8:
  case IR_OP_SIMD_SLP_MAC_I32: {
    /* Inline SLP MAC kernel. Marshal the three effective element pointers
     * (base + offset*4), the k count, and the byte row stride into
     * RCX/RDX/R8/R9/RAX — like call-argument setup — then emit the pure-loop MIR
     * op. The lane count K is a compile-time constant (validated in
     * eligibility); the kernel advances b by the RAX stride each iteration. The
     * op is treated like a call by the allocator, so no live value occupies a
     * volatile across it.
     *
     * Compute every value into a vreg FIRST, then do all the fixed-register MOVs
     * LAST: the MIR_LEA encoder stages spilled base/index through RDX/R11, which
     * would otherwise clobber a kernel argument already parked in RDX. */
    long long K = in->arguments[0].int_value;
    /* Element size per pointer. int32 SLP: a/b/out all 4-byte. int8 SLP: a and
     * b are int8 arrays (1-byte), out (c) is int32 (4-byte). The stride (b's
     * per-k row advance) is in the same units as b, so it scales by b's element
     * size. The MIR op's `width` carries b's element size so the encoder picks
     * the int8-widening kernel. */
    int is_i8 = (in->op == IR_OP_SIMD_SLP_MAC_I8);
    const int elem[3] = {is_i8 ? 1 : 4, is_i8 ? 1 : 4, 4}; /* a, b, out */
    const IROperand *bases[3] = {&in->lhs, &in->rhs, &in->dest}; /* a, b, out */
    const int off_arg[3] = {2, 3, 5};
    MirVregId ptr_vreg[3];
    for (int p = 0; p < 3; p++) {
      MirOperand base = mir_value_operand(fn, g, ctx, map, bases[p]);
      MirOperand off = mir_value_operand(fn, g, ctx, map, &in->arguments[off_arg[p]]);
      if (base.kind != MIR_OPK_VREG) {
        fn->has_error = 1;
        return 0;
      }
      ptr_vreg[p] = mir_new_vreg(fn, MIR_RC_GP, 8);
      if (ptr_vreg[p] == MIR_VREG_NONE) {
        return 0;
      }
      MirOperand mem;
      if (off.kind == MIR_OPK_IMM) {
        mem = mir_op_mem_vreg(base.vreg, MIR_VREG_NONE, 0,
                              (int)(off.imm * elem[p]));
      } else if (off.kind == MIR_OPK_VREG) {
        mem = mir_op_mem_vreg(base.vreg, off.vreg, elem[p], 0);
      } else {
        fn->has_error = 1;
        return 0;
      }
      if (!mir_emit1(fn, MIR_LEA, mir_op_vreg(ptr_vreg[p]), mem, mir_op_none(),
                     8, 0, 0)) {
        return 0;
      }
    }
    /* byte row stride into a vreg (stride_elems * b's element size). */
    int stride_elem = elem[1];
    MirOperand stride = mir_value_operand(fn, g, ctx, map, &in->arguments[4]);
    MirVregId stride_vreg = mir_new_vreg(fn, MIR_RC_GP, 8);
    if (stride_vreg == MIR_VREG_NONE) {
      return 0;
    }
    if (stride.kind == MIR_OPK_IMM) {
      if (!mir_emit1(fn, MIR_MOV, mir_op_vreg(stride_vreg),
                     mir_op_imm(stride.imm * stride_elem), mir_op_none(), 8, 0,
                     0)) {
        return 0;
      }
    } else if (stride.kind == MIR_OPK_VREG && stride_elem == 4) {
      if (!mir_emit1(fn, MIR_SHL, mir_op_vreg(stride_vreg), stride,
                     mir_op_imm(2), 8, 0, 0)) {
        return 0;
      }
    } else if (stride.kind == MIR_OPK_VREG) { /* stride_elem == 1: no scaling */
      if (!mir_emit1(fn, MIR_MOV, mir_op_vreg(stride_vreg), stride,
                     mir_op_none(), 8, 0, 0)) {
        return 0;
      }
    } else {
      fn->has_error = 1;
      return 0;
    }
    MirOperand cnt = mir_value_operand(fn, g, ctx, map, &in->arguments[1]);
    MirVregId cnt_vreg = mir_new_vreg(fn, MIR_RC_GP, 8);
    if (cnt_vreg == MIR_VREG_NONE ||
        !mir_emit1(fn, MIR_MOV, mir_op_vreg(cnt_vreg), cnt, mir_op_none(), 8, 0,
                   0)) {
      return 0;
    }
    /* Now park each computed value in its kernel register (no LEAs left to
     * clobber them). RCX=a, RDX=b, R8=out, R9=count, RAX=byte stride. */
    if (!mir_emit1(fn, MIR_MOV, mir_op_phys(BINARY_GP_RCX, MIR_RC_GP),
                   mir_op_vreg(ptr_vreg[0]), mir_op_none(), 8, 0, 0) ||
        !mir_emit1(fn, MIR_MOV, mir_op_phys(BINARY_GP_RDX, MIR_RC_GP),
                   mir_op_vreg(ptr_vreg[1]), mir_op_none(), 8, 0, 0) ||
        !mir_emit1(fn, MIR_MOV, mir_op_phys(BINARY_GP_R8, MIR_RC_GP),
                   mir_op_vreg(ptr_vreg[2]), mir_op_none(), 8, 0, 0) ||
        !mir_emit1(fn, MIR_MOV, mir_op_phys(BINARY_GP_R9, MIR_RC_GP),
                   mir_op_vreg(cnt_vreg), mir_op_none(), 8, 0, 0) ||
        !mir_emit1(fn, MIR_MOV, mir_op_phys(BINARY_GP_RAX, MIR_RC_GP),
                   mir_op_vreg(stride_vreg), mir_op_none(), 8, 0, 0)) {
      return 0;
    }
    /* width = b's element size (1 = int8-widening kernel, 4 = int32 kernel). */
    return mir_emit1(fn, MIR_SIMD_SLP_MAC, mir_op_imm(K), mir_op_none(),
                     mir_op_none(), elem[1], 0, 0);
  }

  case IR_OP_SIMD_FILL: {
    /* Inline element-counted fill (gated to the mode-0/no-offset/no-writeback,
     * integer-value subset). Marshal base->RCX, element count->R8, value->RAX,
     * then emit the kernel. The value is parked into RAX LAST so it cannot
     * clobber a base/count source that the allocator happened to place in RAX
     * (the only poolable register among the three targets). */
    MirOperand base = mir_value_operand(fn, g, ctx, map, &in->lhs);
    MirOperand cnt = mir_value_operand(fn, g, ctx, map, &in->rhs);
    MirOperand val = mir_value_operand(fn, g, ctx, map, &in->arguments[2]);
    long long size = in->arguments[0].int_value;
    long long mode = in->arguments[1].int_value;
    /* Mode-0 runtime offset (`base[offset + i]`, start 0, int64 index): fold the
     * effective base `base + offset*size` here in 64-bit MIR so the kernel runs
     * the plain element loop. The count (rhs) is the element length unchanged. */
    if (mode == 0 && !(in->arguments[4].kind == IR_OPERAND_INT &&
                       in->arguments[4].int_value == 0)) {
      MirOperand off = mir_value_operand(fn, g, ctx, map, &in->arguments[4]);
      MirVregId scaled = mir_new_vreg(fn, MIR_RC_GP, 8);
      if (scaled == MIR_VREG_NONE) {
        return 0;
      }
      int shift = (size == 8) ? 3 : (size == 4) ? 2 : (size == 2) ? 1 : 0;
      if (shift > 0) {
        if (!mir_emit1(fn, MIR_SHL, mir_op_vreg(scaled), off, mir_op_imm(shift),
                       8, 0, 0)) {
          return 0;
        }
      } else if (!mir_emit1(fn, MIR_MOV, mir_op_vreg(scaled), off, mir_op_none(),
                            8, 0, 0)) {
        return 0;
      }
      MirVregId adj = mir_new_vreg(fn, MIR_RC_GP, 8);
      if (adj == MIR_VREG_NONE ||
          !mir_emit1(fn, MIR_ADD, mir_op_vreg(adj), base, mir_op_vreg(scaled), 8,
                     0, 0)) {
        return 0;
      }
      base = mir_op_vreg(adj);
    }
    if (!mir_emit1(fn, MIR_MOV, mir_op_phys(BINARY_GP_RCX, MIR_RC_GP), base,
                   mir_op_none(), 8, 0, 0) ||
        !mir_emit1(fn, MIR_MOV, mir_op_phys(BINARY_GP_R8, MIR_RC_GP), cnt,
                   mir_op_none(), 8, 0, 0) ||
        !mir_emit1(fn, MIR_MOV, mir_op_phys(BINARY_GP_RAX, MIR_RC_GP), val,
                   mir_op_none(), 8, 0, 0)) {
      return 0;
    }
    /* dst.imm = element size; a.imm = fill mode (0 element-counted, 1 byte-walk). */
    if (!mir_emit1(fn, MIR_SIMD_FILL, mir_op_imm(size), mir_op_imm(mode),
                   mir_op_none(), (int)size, 0, 0)) {
      return 0;
    }
    /* Live induction variable (mode 0, start 0): the unit-stride loop leaves
     * iv = max(count, 0) (the count for an empty loop, else the bound). Fold it
     * branchlessly as `cnt & ~(cnt >> 63)` so a later use of the counter reads
     * the right value -- matching the fallback's cmov write-back exactly. */
    if (mode == 0 && in->dest.kind == IR_OPERAND_SYMBOL) {
      MirOperand iv = mir_value_operand(fn, g, ctx, map, &in->dest);
      MirVregId mask = mir_new_vreg(fn, MIR_RC_GP, 8);
      if (mask == MIR_VREG_NONE ||
          !mir_emit1(fn, MIR_SAR, mir_op_vreg(mask), cnt, mir_op_imm(63), 8, 0,
                     0) ||
          !mir_emit1(fn, MIR_NOT, mir_op_vreg(mask), mir_op_vreg(mask),
                     mir_op_none(), 8, 0, 0) ||
          !mir_emit1(fn, MIR_AND, iv, cnt, mir_op_vreg(mask), 8, 0, 0)) {
        return 0;
      }
    }
    return 1;
  }

  case IR_OP_SIMD_AFFINE_MAP_F32: {
    /* Inline float32 affine map: marshal src->RCX, dst->RDX, count->R8, then emit
     * the kernel with the (compile-time) a/b/c coefficient bits in dst/a/b.imm
     * and the b_is_one/b_is_zero/c_is_zero flags in cc. */
    MirOperand src = mir_value_operand(fn, g, ctx, map, &in->lhs);
    MirOperand dst = mir_value_operand(fn, g, ctx, map, &in->rhs);
    MirOperand cnt = mir_value_operand(fn, g, ctx, map, &in->arguments[0]);
    if (!mir_emit1(fn, MIR_MOV, mir_op_phys(BINARY_GP_RCX, MIR_RC_GP), src,
                   mir_op_none(), 8, 0, 0) ||
        !mir_emit1(fn, MIR_MOV, mir_op_phys(BINARY_GP_RDX, MIR_RC_GP), dst,
                   mir_op_none(), 8, 0, 0) ||
        !mir_emit1(fn, MIR_MOV, mir_op_phys(BINARY_GP_R8, MIR_RC_GP), cnt,
                   mir_op_none(), 8, 0, 0)) {
      return 0;
    }
    long long a_bits = (long long)(uint32_t)mir_float_bits_at(
        in->arguments[1].float_value, 4);
    long long b_bits = (long long)(uint32_t)mir_float_bits_at(
        in->arguments[2].float_value, 4);
    long long c_bits = (long long)(uint32_t)mir_float_bits_at(
        in->arguments[3].float_value, 4);
    int b_is_one = in->arguments[2].float_value == 1.0;
    int b_is_zero = in->arguments[2].float_value == 0.0;
    int c_is_zero = in->arguments[3].float_value == 0.0;
    unsigned char flags = (unsigned char)((b_is_one ? 1 : 0) |
                                          (b_is_zero ? 2 : 0) |
                                          (c_is_zero ? 4 : 0));
    return mir_emit1(fn, MIR_SIMD_AFFINE_MAP_F32, mir_op_imm(a_bits),
                     mir_op_imm(b_bits), mir_op_imm(c_bits), 4, 0, flags);
  }

  case IR_OP_SIMD_AFFINE_MAP_F64: {
    /* Inline float64 affine map: marshal src->RCX, dst->RDX, count->R8, then
     * emit the kernel with the (compile-time) a/b/c coefficient 64-bit bits in
     * dst/a/b.imm and the b_is_one/b_is_zero/c_is_zero flags in cc. */
    MirOperand src = mir_value_operand(fn, g, ctx, map, &in->lhs);
    MirOperand dst = mir_value_operand(fn, g, ctx, map, &in->rhs);
    MirOperand cnt = mir_value_operand(fn, g, ctx, map, &in->arguments[0]);
    if (!mir_emit1(fn, MIR_MOV, mir_op_phys(BINARY_GP_RCX, MIR_RC_GP), src,
                   mir_op_none(), 8, 0, 0) ||
        !mir_emit1(fn, MIR_MOV, mir_op_phys(BINARY_GP_RDX, MIR_RC_GP), dst,
                   mir_op_none(), 8, 0, 0) ||
        !mir_emit1(fn, MIR_MOV, mir_op_phys(BINARY_GP_R8, MIR_RC_GP), cnt,
                   mir_op_none(), 8, 0, 0)) {
      return 0;
    }
    /* Runtime `a`: marshal its scalar value into XMM4 (the kernel's `a` lane,
     * a caller-saved register the kernel clobbers anyway) right before the
     * kernel, which then broadcasts it instead of materializing an immediate. */
    int a_runtime = in->arguments[1].kind != IR_OPERAND_FLOAT;
    if (a_runtime) {
      MirOperand av = mir_value_operand(fn, g, ctx, map, &in->arguments[1]);
      if (!mir_emit_fmov(fn, mir_op_phys(BINARY_XMM4, MIR_RC_XMM), av, 8)) {
        return 0;
      }
    }
    long long a_bits =
        a_runtime ? 0
                  : (long long)mir_float_bits_at(in->arguments[1].float_value, 8);
    long long b_bits = (long long)mir_float_bits_at(in->arguments[2].float_value, 8);
    long long c_bits = (long long)mir_float_bits_at(in->arguments[3].float_value, 8);
    int b_is_one = in->arguments[2].float_value == 1.0;
    int b_is_zero = in->arguments[2].float_value == 0.0;
    int c_is_zero = in->arguments[3].float_value == 0.0;
    unsigned char flags = (unsigned char)((b_is_one ? 1 : 0) |
                                          (b_is_zero ? 2 : 0) |
                                          (c_is_zero ? 4 : 0) |
                                          (a_runtime ? 8 : 0));
    return mir_emit1(fn, MIR_SIMD_AFFINE_MAP_F64, mir_op_imm(a_bits),
                     mir_op_imm(b_bits), mir_op_imm(c_bits), 8, 0, flags);
  }

  case IR_OP_SIMD_VLOOP_F64: {
    /* Inline general vloop (float64 map): marshal the <=3 distinct base pointers
     * into RCX/RDX/R8/R9 (kGp order, matching the kernel's dist) and the element
     * count into the next arg register; the kernel reads its DAG from the
     * borrowed IRInstruction in `aux`. */
    static const int kGp[4] = {BINARY_GP_RCX, BINARY_GP_RDX, BINARY_GP_R8,
                               BINARY_GP_R9};
    const char *vnames[4];
    const IROperand *vsrcs[4];
    int vn = 0;
    if (code_generator_vloop_collect_dist(in, 0, vnames, vsrcs, &vn) < 0 ||
        vn > 3) {
      return 0;
    }
    for (int vk = 0; vk < vn; vk++) {
      MirOperand v = mir_value_operand(fn, g, ctx, map, vsrcs[vk]);
      if (!mir_emit1(fn, MIR_MOV, mir_op_phys(kGp[vk], MIR_RC_GP), v,
                     mir_op_none(), 8, 0, 0)) {
        return 0;
      }
    }
    MirOperand cnt = mir_value_operand(fn, g, ctx, map, &in->lhs);
    if (!mir_emit1(fn, MIR_MOV, mir_op_phys(kGp[vn], MIR_RC_GP), cnt,
                   mir_op_none(), 8, 0, 0)) {
      return 0;
    }
    MirInst v;
    memset(&v, 0, sizeof(v));
    v.op = MIR_SIMD_VLOOP;
    v.ir_index = -1;
    v.aux = in; /* borrowed: the IR outlives this function's codegen */
    return mir_emit(fn, &v);
  }

  case IR_OP_SIMD_SILU_F32: {
    /* Inline SiLU/SwiGLU gate: marshal g/out->RCX, count->R8, u->RDX (SwiGLU),
     * then emit the kernel with has_mul in dst.imm. */
    int has_mul = (in->rhs.kind == IR_OPERAND_TEMP ||
                   in->rhs.kind == IR_OPERAND_SYMBOL);
    MirOperand gbase = mir_value_operand(fn, g, ctx, map, &in->lhs);
    MirOperand cnt = mir_value_operand(fn, g, ctx, map, &in->arguments[0]);
    if (!mir_emit1(fn, MIR_MOV, mir_op_phys(BINARY_GP_RCX, MIR_RC_GP), gbase,
                   mir_op_none(), 8, 0, 0) ||
        !mir_emit1(fn, MIR_MOV, mir_op_phys(BINARY_GP_R8, MIR_RC_GP), cnt,
                   mir_op_none(), 8, 0, 0)) {
      return 0;
    }
    if (has_mul) {
      MirOperand ubase = mir_value_operand(fn, g, ctx, map, &in->rhs);
      if (!mir_emit1(fn, MIR_MOV, mir_op_phys(BINARY_GP_RDX, MIR_RC_GP), ubase,
                     mir_op_none(), 8, 0, 0)) {
        return 0;
      }
    }
    return mir_emit1(fn, MIR_SIMD_SILU_F32, mir_op_imm(has_mul ? 1 : 0),
                     mir_op_none(), mir_op_none(), 4, 0, 0);
  }

  case IR_OP_ADDRESS_OF: {
    MirOperand dst = mir_value_operand(fn, g, ctx, map, &in->dest);
    const IRFunction *irf =
        ctx && ctx->function_name
            ? code_generator_find_ir_function_binary(g, ctx->function_name)
            : NULL;
    MirAddrofKind ak = mir_addressof_kind(g, irf, in);
    if (ak == MIR_ADDROF_INDIRECT_PARAM) {
      /* &@p of a by-reference (INDIRECT) struct param: the param already holds
       * the struct's address, so the address-of is just a copy of the pointer. */
      MirOperand ptr = mir_value_operand(fn, g, ctx, map, &in->lhs);
      return mir_emit1(fn, MIR_MOV, dst, ptr, mir_op_none(), 8, 0, 0);
    }
    if (ak == MIR_ADDROF_GLOBAL) {
      /* &global: lea its RIP-relative address (is_unsigned carries the
       * declare-external flag for the encoder). The global stays cached; the
       * main loop flushes/reloads address-taken globals around pointer memory
       * ops so the alias and the cache vreg stay coherent. */
      Symbol *s = g->symbol_table
                      ? symbol_table_lookup(g->symbol_table, in->lhs.name)
                      : NULL;
      int is_extern = (s && s->is_extern) ? 1 : 0;
      return mir_emit1(fn, MIR_LEA_GLOBAL, dst, mir_op_symbol(in->lhs.name),
                       mir_op_none(), 8, is_extern, 0);
    }
    if (ak == MIR_ADDROF_FUNCTION) {
      Symbol *s = g->symbol_table
                      ? symbol_table_lookup(g->symbol_table, in->lhs.name)
                      : NULL;
      int is_extern = (s && s->is_extern) ? 1 : 0;
      return mir_emit1(fn, MIR_LEA_FUNC, dst, mir_op_symbol(in->lhs.name),
                       mir_op_none(), 8, is_extern, 0);
    }
    /* &local / &param: mark the target memory-resident and lea its stack home. */
    MirOperand src = mir_value_operand(fn, g, ctx, map, &in->lhs);
    if (src.kind != MIR_OPK_VREG) {
      fn->has_error = 1;
      return 0;
    }
    fn->vregs[src.vreg].address_taken = 1;
    /* An INDIRECT struct local needs a home large enough for the whole struct,
     * since field stores reach past the first 8 bytes. Size it to the struct
     * size rounded up to an 8-byte slot. (Scalars and DIRECT small aggregates
     * keep home_bytes == 0, i.e. the default single slot.) The type is resolved
     * from the IR (function scope has popped from the symbol table by now). */
    {
      int is_param = 0;
      Type *lt = mir_local_or_param_type(g, irf, in->lhs.name, &is_param);
      if (lt && !is_param && code_generator_type_is_aggregate(lt) &&
          code_generator_abi_classify(lt) == ABI_PASS_INDIRECT) {
        size_t sz = code_generator_abi_type_size(lt);
        fn->vregs[src.vreg].home_bytes = (int)((sz + 7) & ~(size_t)7);
      }
    }
    return mir_emit1(fn, MIR_LEA_LOCAL, dst, src, mir_op_none(), 8, 0, 0);
  }

  default:
    fn->has_error = 1;
    return 0;
  }
}

/* ---- scaled-address (SIB) folding --------------------------------------- *
 * An array access lowers to three IR ops: a shift/multiply that scales the
 * index, an add that offsets the base pointer, and the load/store itself. x86
 * addresses that whole thing in one [base + index*scale] memory operand, so we
 * detect the pattern and let the load/store carry a SIB MirMem, dropping the
 * two address-computation instructions. This is the single biggest scalar
 * codegen win for index-heavy loops (e.g. matmul): it removes a shift, an add,
 * and (when the base would otherwise spill) a reload every memory access. */

typedef struct {
  int valid;
  IROperand base;
  IROperand index;
  int scale;
} MirAddrFold;

/* Number of instructions that READ temp `name`: any lhs/rhs operand, plus a
 * STORE's dest (its address). A producer's own dest is a definition, not a
 * read, so it is excluded. Used to confirm an address sub-expression feeds
 * nothing but the access before its producer is dropped. */
static int mir_temp_read_count(const IRFunction *f, const char *name) {
  int n = 0;
  for (size_t i = 0; i < f->instruction_count; i++) {
    const IRInstruction *in = &f->instructions[i];
    if (in->lhs.kind == IR_OPERAND_TEMP && in->lhs.name &&
        strcmp(in->lhs.name, name) == 0) {
      n++;
    }
    if (in->rhs.kind == IR_OPERAND_TEMP && in->rhs.name &&
        strcmp(in->rhs.name, name) == 0) {
      n++;
    }
    if (in->op == IR_OP_STORE && in->dest.kind == IR_OPERAND_TEMP &&
        in->dest.name && strcmp(in->dest.name, name) == 0) {
      n++;
    }
  }
  return n;
}

/* Index of the instruction whose dest defines temp `name`, or -1. */
static long mir_temp_def_index(const IRFunction *f, const char *name) {
  for (size_t i = 0; i < f->instruction_count; i++) {
    const IRInstruction *in = &f->instructions[i];
    if (in->dest.kind == IR_OPERAND_TEMP && in->dest.name &&
        strcmp(in->dest.name, name) == 0) {
      return (long)i;
    }
  }
  return -1;
}

/* If `p` scales an index by a legal SIB factor (`idx << k`, k in 0..3, or
 * `idx * c`, c in {1,2,4,8}), fill *index/*scale and return 1. */
static int mir_decode_scale(const IRInstruction *p, IROperand *index,
                            int *scale) {
  if (p->op != IR_OP_BINARY || p->is_float || !p->text) {
    return 0;
  }
  if (strcmp(p->text, "<<") == 0 && p->rhs.kind == IR_OPERAND_INT) {
    long long k = p->rhs.int_value;
    if (k < 0 || k > 3) {
      return 0;
    }
    *index = p->lhs;
    *scale = 1 << k;
    return 1;
  }
  if (strcmp(p->text, "*") == 0) {
    const IROperand *konst = NULL, *var = NULL;
    if (p->rhs.kind == IR_OPERAND_INT) {
      konst = &p->rhs;
      var = &p->lhs;
    } else if (p->lhs.kind == IR_OPERAND_INT) {
      konst = &p->lhs;
      var = &p->rhs;
    } else {
      return 0;
    }
    long long c = konst->int_value;
    if (c == 1 || c == 2 || c == 4 || c == 8) {
      *index = *var;
      *scale = (int)c;
      return 1;
    }
  }
  return 0;
}

/* Scan for `LOAD/STORE [ base + (index<<k|index*c) ]` and record a SIB fold for
 * each, marking the two address-producer instructions to be skipped. Only
 * integer accesses fold (the float encoder path does not read mem.index). */
/* Mark for skipping the producer of any loop-bound constant that the fused
 * compare-branch will fold into an imm32 (see mir_fused_cmp_imm). Without this
 * the CAST that materializes the bound stays in the loop as a dead `mov reg,
 * imm` every iteration. Only drops a producer whose temp is read solely by that
 * compare. */
static void mir_compute_const_compare_skips(CodeGenerator *g,
                                            BinaryFunctionContext *ctx,
                                            IRFunction *f, char *skip) {
  for (size_t i = 0; i + 1 < f->instruction_count; i++) {
    if (!mir_fuses_compare_branch(g, f, i)) {
      continue;
    }
    const IRInstruction *cmp = &f->instructions[i];
    if (cmp->is_float || cmp->rhs.kind != IR_OPERAND_TEMP || !cmp->rhs.name) {
      continue;
    }
    long long imm;
    if (!mir_fused_cmp_imm(g, ctx, f, &cmp->rhs, &imm)) {
      continue;
    }
    if (mir_temp_read_count(f, cmp->rhs.name) != 1) {
      continue; /* bound temp feeds something else; keep its producer */
    }
    for (size_t j = 0; j < f->instruction_count; j++) {
      const IRInstruction *in = &f->instructions[j];
      if (in->dest.kind == IR_OPERAND_TEMP && in->dest.name &&
          strcmp(in->dest.name, cmp->rhs.name) == 0) {
        skip[j] = 1;
        break;
      }
    }
  }
}

static void mir_compute_address_folds(const IRFunction *f, char *skip,
                                      MirAddrFold *folds) {
  for (size_t i = 0; i < f->instruction_count; i++) {
    const IRInstruction *in = &f->instructions[i];
    const IROperand *addr;
    if (in->op == IR_OP_LOAD) {
      addr = &in->lhs;
    } else if (in->op == IR_OP_STORE) {
      addr = &in->dest;
    } else {
      continue;
    }
    if (in->is_float || addr->kind != IR_OPERAND_TEMP || !addr->name) {
      continue;
    }
    /* The address must feed only this access, or dropping its producer would
     * lose a value another instruction needs. */
    if (mir_temp_read_count(f, addr->name) != 1) {
      continue;
    }
    long ai = mir_temp_def_index(f, addr->name);
    if (ai < 0) {
      continue;
    }
    const IRInstruction *padd = &f->instructions[ai];
    if (padd->op != IR_OP_BINARY || padd->is_float || !padd->text ||
        strcmp(padd->text, "+") != 0) {
      continue;
    }
    /* One operand is the base pointer, the other the scaled index (a temp whose
     * sole use is this add). Try both orderings. */
    const IROperand *order[2][2] = {{&padd->lhs, &padd->rhs},
                                    {&padd->rhs, &padd->lhs}};
    for (int t = 0; t < 2; t++) {
      const IROperand *base = order[t][0];
      const IROperand *scaled = order[t][1];
      if (scaled->kind != IR_OPERAND_TEMP || !scaled->name) {
        continue;
      }
      if (mir_temp_read_count(f, scaled->name) != 1) {
        continue;
      }
      long si = mir_temp_def_index(f, scaled->name);
      if (si < 0) {
        continue;
      }
      IROperand index;
      int scale;
      if (!mir_decode_scale(&f->instructions[si], &index, &scale)) {
        continue;
      }
      folds[i].valid = 1;
      folds[i].base = *base;
      folds[i].index = index;
      folds[i].scale = scale;
      skip[ai] = 1; /* the base+scaled add */
      skip[si] = 1; /* the index scale */
      break;
    }

    /* Scale-1 fallback: a plain `base + index` with no explicit scaling, i.e.
     * the unit-stride access `a[i]` on a byte/char/pointer-sized-by-1 buffer
     * (and any loop walking an int8/uint8 array). Both operands must be
     * register-resident values (TEMP or SYMBOL); fold them straight into
     * [op0 + op1*1]. base/index are symmetric at scale 1, so either ordering
     * encodes identically. Unlike the scaled path this consumes no separate
     * producer and leaves both operands live (the index is typically the loop
     * induction variable, still needed by the increment), so only the add
     * itself is dropped. */
    if (!folds[i].valid) {
      const IROperand *o0 = &padd->lhs;
      const IROperand *o1 = &padd->rhs;
      int o0_reg = (o0->kind == IR_OPERAND_TEMP || o0->kind == IR_OPERAND_SYMBOL);
      int o1_reg = (o1->kind == IR_OPERAND_TEMP || o1->kind == IR_OPERAND_SYMBOL);
      if (o0_reg && o1_reg) {
        folds[i].valid = 1;
        folds[i].base = *o0;
        folds[i].index = *o1;
        folds[i].scale = 1;
        skip[ai] = 1; /* fold the base+index add into the memory operand */
      }
    }

    /* Constant-displacement fallback: `ptr + const_int` -- a struct-field or
     * fixed-offset access (`p->field`, `b[i].field`, `*(ptr + k)`) -- folds the
     * constant straight into the x86 displacement: [base + disp]. The constant
     * is exactly the displacement (the access size is unchanged, so there is no
     * width or aliasing concern). Unlike the scaled/scale-1 paths this consumes
     * no separate producer -- only the add is dropped -- and the base pointer
     * temp stays live, since it is typically shared across several field
     * accesses on the same element (folding base+index*scale here instead would
     * re-derive it per field). mir_lower_folded_access turns an IR_OPERAND_INT
     * index into the displacement. */
    if (!folds[i].valid) {
      const IROperand *o0 = &padd->lhs;
      const IROperand *o1 = &padd->rhs;
      const IROperand *base = NULL;
      const IROperand *cst = NULL;
      if (o0->kind == IR_OPERAND_TEMP && o0->name && o1->kind == IR_OPERAND_INT) {
        base = o0;
        cst = o1;
      } else if (o1->kind == IR_OPERAND_TEMP && o1->name &&
                 o0->kind == IR_OPERAND_INT) {
        base = o1;
        cst = o0;
      }
      if (base && cst->int_value >= -2147483648LL &&
          cst->int_value <= 2147483647LL) {
        folds[i].valid = 1;
        folds[i].base = *base;
        folds[i].index = *cst;
        folds[i].scale = 1;
        skip[ai] = 1; /* fold the ptr+const add into the memory displacement */
      }
    }
  }
}

/* Lower a LOAD/STORE whose address folded into a [base + index*scale] SIB. */
static int mir_lower_folded_access(MirFunction *fn, CodeGenerator *g,
                                   BinaryFunctionContext *ctx, MirNameMap *map,
                                   const IRInstruction *in,
                                   const MirAddrFold *fold) {
  MirOperand baseo = mir_value_operand(fn, g, ctx, map, &fold->base);
  if (baseo.kind != MIR_OPK_VREG) {
    fn->has_error = 1;
    return 0;
  }
  MirOperand mem;
  if (fold->index.kind == IR_OPERAND_INT) {
    /* A constant index (e.g. `p[0]`, `arr[5]`) folds into the displacement:
     * [base + index*scale]. mir_decode_scale yields the literal index when the
     * scaled-offset expression is itself constant. */
    long long disp = fold->index.int_value * (long long)fold->scale;
    if (disp < -2147483648LL || disp > 2147483647LL) {
      fn->has_error = 1;
      return 0;
    }
    mem = mir_op_mem_vreg(baseo.vreg, MIR_VREG_NONE, 0, (int)disp);
  } else {
    MirOperand idxo = mir_value_operand(fn, g, ctx, map, &fold->index);
    if (idxo.kind != MIR_OPK_VREG) {
      fn->has_error = 1;
      return 0;
    }
    mem = mir_op_mem_vreg(baseo.vreg, idxo.vreg, fold->scale, 0);
  }
  int size = code_generator_binary_get_access_size(g, ctx, &in->rhs);
  if (size <= 0) {
    fn->has_error = 1;
    return 0;
  }
  if (in->op == IR_OP_LOAD) {
    MirOperand dst = mir_value_operand(fn, g, ctx, map, &in->dest);
    int sign_ext = !in->is_unsigned &&
                   code_generator_binary_load_needs_sign_extend(g, ctx,
                                                               &in->dest, size);
    return mir_emit1(fn, MIR_MOV, dst, mem, mir_op_none(), size,
                     sign_ext ? 0 : 1, 0);
  }
  MirOperand val = mir_value_operand(fn, g, ctx, map, &in->lhs);
  return mir_emit1(fn, MIR_MOV, mem, val, mir_op_none(), size, 0, 0);
}

/* ---- loop rotation ------------------------------------------------------ */

/* Rotate top-tested loops to bottom-tested ones. The lowering emits a while
 * loop as `label H; CMPBR cc -> Lexit; <body>; JMP H` — a fall-through test at
 * the top plus an unconditional back-jump every iteration (two branches/iter).
 * This rewrites it to `CMPBR cc -> Lexit (guard); H: <body>; CMPBR !cc -> H`,
 * so the back-edge is a single conditional branch and the top test runs once.
 *
 * Done by (1) converting each backward `JMP H` into a `CMPBR` with the header's
 * compare operands and the inverted condition (x86: cc ^ 1 flips the test),
 * targeting H, and (2) swapping `label H` with its following CMPBR so H now
 * marks the body start. Only safe when H is immediately followed by its CMPBR:
 * then the compare operands are loop-stable live values (a counter and a bound),
 * not temps computed by the header's condition evaluation — so re-testing them
 * at the back-edge (after the body's update) is exactly the loop condition. */
/* Fuse `MOV d, s` immediately followed by `MOVSX/MOVZX d, d` into
 * `MOVSX/MOVZX d, s`: the extend overwrites d with the extension of its low
 * bytes, so reading s directly is identical and the copy is dead. The narrow-
 * integer canonicalization emitted after an ASSIGN is exactly this shape
 * (`MOV cd, b; MOVSX cd, cd`), so this removes a register copy -- and one
 * short-lived value, easing register pressure -- per narrow copy-assign, which
 * is common in inlined and recursive code (e.g. rec_fib's `mov r13,r12; movsxd
 * r13,r13d`). Only vreg->vreg moves (a load/immediate MOV is left alone), and
 * the two are adjacent so s cannot be redefined between them. */
static void mir_fuse_mov_then_extend(MirFunction *fn) {
  if (!fn) {
    return;
  }
  for (size_t i = 1; i < fn->insn_count; i++) {
    MirInst *ext = &fn->insns[i];
    if ((ext->op != MIR_MOVSX && ext->op != MIR_MOVZX) || ext->is_float ||
        ext->dst.kind != MIR_OPK_VREG || ext->a.kind != MIR_OPK_VREG ||
        ext->dst.vreg != ext->a.vreg) {
      continue;
    }
    MirInst *mov = &fn->insns[i - 1];
    if (mov->op != MIR_MOV || mov->is_float || mov->dst.kind != MIR_OPK_VREG ||
        mov->dst.vreg != ext->dst.vreg || mov->a.kind != MIR_OPK_VREG ||
        mov->a.vreg == ext->dst.vreg) {
      continue;
    }
    ext->a.vreg = mov->a.vreg; /* extend reads the copy's source directly */
    mov->op = MIR_NOP;         /* the copy is now dead */
  }
}

static void mir_rotate_loops(MirFunction *fn) {
  if (!fn || fn->insn_count < 3) {
    return;
  }
  for (size_t j = 0; j + 1 < fn->insn_count; j++) {
    /* A rotatable header is `label H` immediately followed by its `CMPBR cc ->
     * E`. (Immediate adjacency means the compare operands are loop-stable live
     * values, not header-computed temps.) */
    if (fn->insns[j].op != MIR_LABEL ||
        fn->insns[j].dst.kind != MIR_OPK_LABEL || !fn->insns[j].dst.sym ||
        fn->insns[j + 1].op != MIR_CMPBR ||
        fn->insns[j + 1].dst.kind != MIR_OPK_LABEL ||
        !fn->insns[j + 1].dst.sym) {
      continue;
    }
    const char *hname = fn->insns[j].dst.sym;     /* header / body-start label */
    const char *ename = fn->insns[j + 1].dst.sym; /* loop exit target          */

    /* Require exactly one backward edge: a `JMP H` after the header. (A loop
     * with extra back-edges from `continue` is left unrotated.) */
    size_t be = 0;
    int nbe = 0;
    for (size_t k = j + 2; k < fn->insn_count; k++) {
      if (fn->insns[k].op == MIR_JMP &&
          fn->insns[k].dst.kind == MIR_OPK_LABEL && fn->insns[k].dst.sym &&
          strcmp(fn->insns[k].dst.sym, hname) == 0) {
        be = k;
        nbe++;
      }
    }
    if (nbe != 1) {
      continue;
    }
    /* The instruction right after the back-edge must be the header's exit label.
     * Otherwise the rotated loop's fall-through (the not-taken bottom test) would
     * land on the wrong block — e.g. when the loop is the last statement in an
     * `if` and its exit is the enclosing block's end, not a `while_end` here. */
    if (be + 1 >= fn->insn_count || fn->insns[be + 1].op != MIR_LABEL ||
        fn->insns[be + 1].dst.kind != MIR_OPK_LABEL ||
        !fn->insns[be + 1].dst.sym ||
        strcmp(fn->insns[be + 1].dst.sym, ename) != 0) {
      continue;
    }

    /* Convert the back-edge `JMP H` into the bottom test `CMPBR !cc -> H` (loop
     * while the condition still holds; fall through to the exit label when it
     * fails). cc ^ 1 inverts the x86 condition. dst already targets H. */
    fn->insns[be].op = MIR_CMPBR;
    fn->insns[be].a = fn->insns[j + 1].a;
    fn->insns[be].b = fn->insns[j + 1].b;
    fn->insns[be].width = fn->insns[j + 1].width;
    fn->insns[be].is_unsigned = fn->insns[j + 1].is_unsigned;
    fn->insns[be].cc = (unsigned char)(fn->insns[j + 1].cc ^ 1u);

    /* Swap `label H` with its CMPBR so H marks the body and the CMPBR is a
     * one-time entry guard. */
    MirInst tmp = fn->insns[j];
    fn->insns[j] = fn->insns[j + 1];
    fn->insns[j + 1] = tmp;
  }
}

/* MIR index of the LABEL defining `name`, or (size_t)-1. */
static size_t mir_label_index(const MirFunction *fn, const char *name) {
  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    if (in->op == MIR_LABEL && in->dst.kind == MIR_OPK_LABEL && in->dst.sym &&
        strcmp(in->dst.sym, name) == 0) {
      return i;
    }
  }
  return (size_t)-1;
}

/* True if MIR index p sits inside a loop body: some JMP/CMPBR back-edge after p
 * targets a label defined at or before p (it spans p). */
static int mir_index_in_loop(const MirFunction *fn, size_t p) {
  for (size_t k = p + 1; k < fn->insn_count; k++) {
    const MirInst *in = &fn->insns[k];
    if ((in->op == MIR_JMP || in->op == MIR_CMPBR) &&
        in->dst.kind == MIR_OPK_LABEL && in->dst.sym) {
      size_t t = mir_label_index(fn, in->dst.sym);
      if (t != (size_t)-1 && t <= p) {
        return 1;
      }
    }
  }
  return 0;
}

static int mir_operand_uses_vreg(const MirOperand *op, MirVregId v) {
  if (!op || v == MIR_VREG_NONE) {
    return 0;
  }
  if (op->kind == MIR_OPK_VREG) {
    return op->vreg == v;
  }
  if (op->kind == MIR_OPK_MEM) {
    return op->mem.base == v || op->mem.index == v;
  }
  return 0;
}

static int mir_inst_uses_vreg(const MirInst *in, MirVregId v) {
  if (!in) {
    return 0;
  }
  if (mir_operand_uses_vreg(&in->a, v) ||
      mir_operand_uses_vreg(&in->b, v)) {
    return 1;
  }
  return in->dst.kind == MIR_OPK_MEM && mir_operand_uses_vreg(&in->dst, v);
}

static size_t mir_first_use_index(const MirFunction *fn, MirVregId v,
                                  size_t def) {
  for (size_t i = 0; i < fn->insn_count; i++) {
    if (i == def) {
      continue;
    }
    if (mir_inst_uses_vreg(&fn->insns[i], v)) {
      return i;
    }
  }
  return (size_t)-1;
}

static size_t mir_const_def_index(const MirFunction *fn, MirVregId v,
                                  int is_float) {
  for (size_t i = 0; i < fn->insn_count; i++) {
    const MirInst *in = &fn->insns[i];
    if (in->op != MIR_MOV || in->is_float != is_float ||
        in->dst.kind != MIR_OPK_VREG || in->dst.vreg != v) {
      continue;
    }
    if (is_float) {
      if (in->a.kind == MIR_OPK_FIMM) {
        return i;
      }
    } else if (in->a.kind == MIR_OPK_IMM) {
      return i;
    }
  }
  return (size_t)-1;
}

static int mir_label_has_forward_target(const MirFunction *fn, const char *name,
                                        size_t label_index) {
  if (!name) {
    return 0;
  }
  for (size_t i = 0; i < label_index; i++) {
    const MirInst *in = &fn->insns[i];
    if ((in->op == MIR_JMP || in->op == MIR_JCC || in->op == MIR_CMPBR ||
         in->op == MIR_FCMPBR) &&
        in->dst.kind == MIR_OPK_LABEL && in->dst.sym &&
        strcmp(in->dst.sym, name) == 0) {
      return 1;
    }
  }
  return 0;
}

/* True iff every use of `v` lies within the inclusive instruction range
 * [lo, hi]. Used to prove a pooled constant is confined to a single loop body
 * before its materialization is sunk to that loop's header. */
static int mir_all_uses_in_range(const MirFunction *fn, MirVregId v, size_t lo,
                                 size_t hi) {
  for (size_t i = 0; i < fn->insn_count; i++) {
    if (mir_inst_uses_vreg(&fn->insns[i], v) && (i < lo || i > hi)) {
      return 0;
    }
  }
  return 1;
}

/* Returns the loop header to sink the constant to, and its loop-body end via
 * *loop_end. Returns first_use (and leaves *loop_end = first_use) when no
 * enclosing loop encloses first_use -- the caller must not relocate then, since
 * a non-loop position need not dominate the constant's other uses. */
static size_t mir_const_insert_index(const MirFunction *fn, size_t first_use,
                                     size_t *loop_end) {
  size_t insert = first_use;
  *loop_end = first_use;
  for (size_t b = 0; b < fn->insn_count; b++) {
    const MirInst *br = &fn->insns[b];
    if ((br->op != MIR_JMP && br->op != MIR_JCC && br->op != MIR_CMPBR &&
         br->op != MIR_FCMPBR) ||
        br->dst.kind != MIR_OPK_LABEL || !br->dst.sym) {
      continue;
    }
    size_t l = mir_label_index(fn, br->dst.sym);
    if (l == (size_t)-1 || l >= b || first_use < l || first_use > b) {
      continue;
    }
    if (mir_label_has_forward_target(fn, br->dst.sym, l)) {
      continue;
    }
    if (insert == first_use || l > insert) {
      insert = l;
      *loop_end = b;
    }
  }
  return insert;
}

/* Loop-pooled constants are discovered before lowering, so their original MOVs
 * land near function entry. Relocate those materializations to the nearest safe
 * preheader of the loop that first uses them: back-edges jump to the label, so
 * an instruction before that label runs once on loop entry and not per
 * iteration. This keeps magic div/mod constants and pooled float literals out of
 * unrelated setup calls and gives the allocator much shorter live ranges. */
static void mir_place_const_pool(MirFunction *fn) {
  if (!fn || (!fn->fconst_count && !fn->iconst_count) || fn->insn_count == 0) {
    return;
  }
  typedef struct {
    size_t def;
    size_t insert;
    MirInst inst;
  } ConstMove;
  size_t max_moves = fn->fconst_count + fn->iconst_count;
  ConstMove *moves = (ConstMove *)calloc(max_moves, sizeof(ConstMove));
  char *skip = (char *)calloc(fn->insn_count, 1);
  if (!moves || !skip) {
    free(moves);
    free(skip);
    return;
  }
  size_t nmove = 0;

  for (size_t i = 0; i < fn->iconst_count; i++) {
    MirVregId v = fn->iconsts[i].vreg;
    size_t def = mir_const_def_index(fn, v, 0);
    if (def == (size_t)-1) {
      continue;
    }
    size_t first = mir_first_use_index(fn, v, def);
    if (first == (size_t)-1) {
      continue;
    }
    size_t loop_end = first;
    size_t insert = mir_const_insert_index(fn, first, &loop_end);
    if (insert <= def + 1 || insert == first ||
        !mir_all_uses_in_range(fn, v, insert, loop_end)) {
      continue;
    }
    moves[nmove].def = def;
    moves[nmove].insert = insert;
    moves[nmove].inst = fn->insns[def];
    skip[def] = 1;
    nmove++;
  }
  for (size_t i = 0; i < fn->fconst_count; i++) {
    MirVregId v = fn->fconsts[i].vreg;
    size_t def = mir_const_def_index(fn, v, 1);
    if (def == (size_t)-1) {
      continue;
    }
    size_t first = mir_first_use_index(fn, v, def);
    if (first == (size_t)-1) {
      continue;
    }
    size_t loop_end = first;
    size_t insert = mir_const_insert_index(fn, first, &loop_end);
    if (insert <= def + 1 || insert == first ||
        !mir_all_uses_in_range(fn, v, insert, loop_end)) {
      continue;
    }
    moves[nmove].def = def;
    moves[nmove].insert = insert;
    moves[nmove].inst = fn->insns[def];
    skip[def] = 1;
    nmove++;
  }

  if (nmove == 0) {
    free(moves);
    free(skip);
    return;
  }

  MirInst *out = (MirInst *)malloc(fn->insn_count * sizeof(MirInst));
  if (!out) {
    free(moves);
    free(skip);
    return;
  }
  size_t w = 0;
  for (size_t i = 0; i <= fn->insn_count; i++) {
    for (size_t m = 0; m < nmove; m++) {
      if (moves[m].insert == i) {
        out[w++] = moves[m].inst;
      }
    }
    if (i == fn->insn_count) {
      break;
    }
    if (!skip[i]) {
      out[w++] = fn->insns[i];
    }
  }
  free(fn->insns);
  fn->insns = out;
  fn->insn_count = w;
  fn->insn_capacity = fn->insn_count;
  free(moves);
  free(skip);
}

/* Cold-exit sinking. An in-loop early-return guard lowers to a forward CMPBR
 * that skips a short straight-line block ending in RET; the loop continuation
 * is the branch TARGET, so the hot path pays a taken forward branch every
 * iteration (on top of the back-edge -- two taken branches/iter). Invert the
 * branch to jump to the return block, sink that block to the function tail, and
 * fall through to the continuation. The back-edge is then the loop's only taken
 * branch (matching what gcc/clang do for search/validation loops). The move is
 * pure relabel + relocate of a straight-line exit block, so it is value- and
 * control-equivalent regardless of the branch's real probability; the loop gate
 * only restricts WHERE it pays off. */
static void mir_sink_cold_exits(MirFunction *fn) {
  if (!fn || fn->insn_count < 4) {
    return;
  }
  /* An appended tail block must be unreachable by fall-through, so the function
   * must already end in a terminator. */
  MirOpcode last = fn->insns[fn->insn_count - 1].op;
  if (last != MIR_RET && last != MIR_JMP && last != MIR_TRAP) {
    return;
  }

  typedef struct {
    size_t p;      /* the CMPBR to invert */
    size_t lo, hi; /* sunk region [lo, hi) -- ends in RET */
    char *label;   /* fresh target name (owned by fn) */
  } Sink;
  Sink *sinks = NULL;
  size_t nsink = 0, cap = 0;
  char *moved = (char *)calloc(fn->insn_count, 1);
  if (!moved) {
    return;
  }

  for (size_t p = 0; p + 1 < fn->insn_count; p++) {
    const MirInst *br = &fn->insns[p];
    if (br->op != MIR_CMPBR || br->dst.kind != MIR_OPK_LABEL || !br->dst.sym) {
      continue;
    }
    size_t q = mir_label_index(fn, br->dst.sym);
    if (q == (size_t)-1 || q < p + 2) {
      continue; /* backward branch, or empty fall-through region */
    }
    if (q - 1 - p > 16) {
      continue; /* keep this to short early-exit blocks */
    }
    int ok = 1;
    for (size_t r = p + 1; r < q; r++) {
      MirOpcode op = fn->insns[r].op;
      if (op == MIR_LABEL || op == MIR_JMP || op == MIR_JCC ||
          op == MIR_CMPBR || op == MIR_FCMPBR) {
        ok = 0;
        break;
      }
      if (op == MIR_RET && r != q - 1) {
        ok = 0;
        break;
      }
    }
    if (!ok || fn->insns[q - 1].op != MIR_RET) {
      continue;
    }
    if (!mir_index_in_loop(fn, p)) {
      continue;
    }

    char buf[32];
    snprintf(buf, sizeof(buf), ".mcsink_%zu", nsink);
    char *name = mettle_strdup(buf);
    if (!name) {
      continue;
    }
    if (fn->owned_sym_count >= fn->owned_sym_capacity) {
      size_t nc = fn->owned_sym_capacity ? fn->owned_sym_capacity * 2 : 4;
      char **grown = (char **)realloc(fn->owned_syms, nc * sizeof(char *));
      if (!grown) {
        free(name);
        continue;
      }
      fn->owned_syms = grown;
      fn->owned_sym_capacity = nc;
    }
    fn->owned_syms[fn->owned_sym_count++] = name;

    if (nsink >= cap) {
      size_t nc = cap ? cap * 2 : 4;
      Sink *grown = (Sink *)realloc(sinks, nc * sizeof(Sink));
      if (!grown) {
        break;
      }
      sinks = grown;
      cap = nc;
    }
    sinks[nsink].p = p;
    sinks[nsink].lo = p + 1;
    sinks[nsink].hi = q;
    sinks[nsink].label = name;
    nsink++;
    for (size_t r = p + 1; r < q; r++) {
      moved[r] = 1;
    }
  }

  if (nsink == 0) {
    free(moved);
    free(sinks);
    return;
  }

  size_t total = fn->insn_count + nsink; /* one new label per sunk block */
  MirInst *out = (MirInst *)malloc(total * sizeof(MirInst));
  if (!out) {
    free(moved);
    free(sinks);
    return;
  }
  size_t w = 0;
  for (size_t i = 0; i < fn->insn_count; i++) {
    if (moved[i]) {
      continue;
    }
    out[w] = fn->insns[i];
    for (size_t s = 0; s < nsink; s++) {
      if (sinks[s].p == i) {
        out[w].cc = (unsigned char)(out[w].cc ^ 1u);
        out[w].dst = mir_op_label(sinks[s].label);
        break;
      }
    }
    w++;
  }
  for (size_t s = 0; s < nsink; s++) {
    MirInst lbl;
    memset(&lbl, 0, sizeof(lbl));
    lbl.op = MIR_LABEL;
    lbl.dst = mir_op_label(sinks[s].label);
    lbl.ir_index = -1;
    out[w++] = lbl;
    for (size_t r = sinks[s].lo; r < sinks[s].hi; r++) {
      out[w++] = fn->insns[r];
    }
  }

  free(fn->insns);
  fn->insns = out;
  fn->insn_count = w;
  fn->insn_capacity = total;
  free(moved);
  free(sinks);
}

/* ---- emit entry --------------------------------------------------------- */

int code_generator_binary_emit_function_via_mir(
    CodeGenerator *generator, FunctionDeclaration *function_data,
    IRFunction *ir_function, BinaryFunctionContext *context) {
  MirFunction fn;
  MirNameMap map;
  mir_function_init(&fn, context);
  fn.generator = generator;
  memset(&map, 0, sizeof(map));

  /* Globals this function writes: register-promoted (cached at entry, written
   * back before each return). Eligibility has proven these are leaf-function
   * scalar-global writes with no aliasing pointer in scope. */
  MirGlobalWriteback wb = {0};
  size_t wb_cap = 0;
  size_t wb_all_cap = 0;
  size_t wb_at_cap = 0;

  /* MIR owns saved registers and the frame; discard anything the legacy
   * promoter left in the context. */
  context->saved_register_count = 0;
  context->saved_xmm_count = 0;
  context->raw_frame_size = 0;
  context->frame_size = 0;
  context->return_float_bits = 0;
  /* Frame-pointer omission is DISABLED: a controlled A/B (same C baseline)
   * showed it is performance-neutral across the benchmark suite (~0% on ~11
   * benches, +3% on const_mod, but -6% on saxpy and -3% on func_ptr) -- no net
   * win, with downside on a couple of leaf loops, plus the added rsp-addressing
   * complexity. The freed rbp rarely binds and rsp-relative slots cost a SIB
   * byte. Set unconditionally to 0 so the allocator keeps the rbp frame and rbp
   * stays reserved. (The FPO machinery in mir_encode/mir_regalloc is inert while
   * this is 0; opt back in via METTLE_FPO if a future change makes it pay off.) */
  {
    static int fpo = -1;
    if (fpo < 0) {
      fpo = getenv("METTLE_FPO") ? 1 : 0;
    }
    context->omit_frame_pointer = fpo;
  }

  /* Bind parameters to vregs and record their incoming extension. */
  const BinaryAbi *abi = code_generator_binary_active_abi();
  (void)abi;
  for (size_t i = 0; i < function_data->parameter_count; i++) {
    const char *pname = function_data->parameter_names[i];
    Type *pt = code_generator_binary_get_resolved_type(
        generator, function_data->parameter_types
                       ? function_data->parameter_types[i]
                       : NULL,
        0);
    int pfb = pt ? code_generator_binary_resolved_type_float_bits(pt) : 0;
    int is_agg = pt && code_generator_type_is_aggregate(pt);
    int w = pfb ? pfb / 8 : (pt ? code_generator_binary_resolved_type_scalar_size(pt) : 8);
    if (is_agg || (!pfb && w != 1 && w != 2 && w != 4 && w != 8)) {
      /* A DIRECT small aggregate arrives in a full GP register; home all 8 bytes
       * with no integer extension (field access reads within the struct size). */
      w = 8;
    }
    MirVregId v = mir_name_map_get_or_add(&map, &fn, pname, 0,
                                          pfb ? MIR_RC_XMM : MIR_RC_GP,
                                          pfb ? w : 8);
    if (v == MIR_VREG_NONE) {
      goto oom;
    }
    fn.params[fn.param_count].vreg = v;
    fn.params[fn.param_count].arg_index = (int)i;
    fn.params[fn.param_count].width = w;
    fn.params[fn.param_count].is_float = pfb ? 1 : 0;
    fn.params[fn.param_count].is_signed =
        (!is_agg && pt)
            ? code_generator_binary_resolved_type_is_signed_integer(pt)
            : 0;
    fn.param_count++;
  }

  /* INDIRECT struct return: the caller passes a hidden out-pointer (Win64: RCX,
   * SysV: RDI) ahead of the user arguments. Reserve a vreg for it; the prologue
   * homes that register into it (shifting user params up one ABI slot) and each
   * RETURN copies the struct there and returns the pointer in RAX. */
  {
    Type *rt = code_generator_binary_get_resolved_type(
        generator, function_data->return_type, 1);
    if (rt && code_generator_type_is_aggregate(rt) &&
        code_generator_abi_classify(rt) == ABI_PASS_INDIRECT) {
      fn.returns_indirect = 1;
      fn.indirect_return_size = (int)code_generator_abi_type_size(rt);
      fn.indirect_return_vreg = mir_new_vreg(&fn, MIR_RC_GP, 8);
      if (fn.indirect_return_vreg == MIR_VREG_NONE) {
        goto oom;
      }
    } else if (rt && code_generator_binary_resolved_type_float_bits(rt) != 0) {
      fn.float_return_bits = code_generator_binary_resolved_type_float_bits(rt);
    } else if (rt && code_generator_binary_resolved_type_float_bits(rt) == 0 &&
               !code_generator_type_is_aggregate(rt)) {
      /* A narrow integer return (int32/uint32/int16/...) must be canonicalized
       * before `mov rax`: MIR computes in 64-bit, so the value can carry garbage
       * above its width, and the Win64/SysV ABI leaves the high RAX bits
       * undefined for a sub-64-bit return — a caller that uses the full register
       * (e.g. `(int64)narrow_fn()`) would then read the garbage. Record the
       * return width/signedness so the RETURN lowering extends to canonical
       * 64-bit form. */
      int rw = code_generator_binary_resolved_type_scalar_size(rt);
      if (rw == 1 || rw == 2 || rw == 4) {
        fn.scalar_return_width = rw;
        fn.scalar_return_signed =
            code_generator_binary_resolved_type_is_signed_integer(rt);
      }
    }
  }

  /* Cache global scalars: load each referenced global once at entry into a vreg
   * so body references (reads AND writes) resolve to that register instead of a
   * per-use RIP-relative memory access. A read-only global is just cached; a
   * written global is additionally recorded for write-back before each return.
   * Eligibility has proven every global access here is a leaf-function scalar
   * global with no aliasing pointer in scope. Emitted before the body so the
   * cache vreg is defined at index 0 (live across the whole function, like a
   * parameter); the loop-extension in the allocator then keeps it live across
   * loop back-edges. */
  for (size_t i = 0; i < ir_function->instruction_count; i++) {
    const IRInstruction *in = &ir_function->instructions[i];
    /* Record a written global scalar for write-back (deduped). */
    if (in->dest.kind == IR_OPERAND_SYMBOL && in->dest.name &&
        mir_name_is_global_scalar(generator, in->dest.name)) {
      int present = 0;
      for (size_t j = 0; j < wb.count; j++) {
        if (strcmp(wb.names[j], in->dest.name) == 0) {
          present = 1;
          break;
        }
      }
      if (!present) {
        if (wb.count >= wb_cap) {
          size_t nc = wb_cap ? wb_cap * 2 : 4;
          const char **grown =
              (const char **)realloc(wb.names, nc * sizeof(*grown));
          if (!grown) {
            goto oom;
          }
          wb.names = grown;
          wb_cap = nc;
        }
        wb.names[wb.count++] = in->dest.name;
      }
    }
    /* Load each global (read or written) into its cache vreg once at entry.
     * Scans dest/lhs/rhs AND call arguments — a global used only as a call
     * argument (f(g)) must still be loaded, or the value path would resolve it
     * to an undefined vreg holding a stale register. */
    for (int k = 0;; k++) {
      const IROperand *op;
      if (k == 0) {
        op = &in->dest;
      } else if (k == 1) {
        op = &in->lhs;
      } else if (k == 2) {
        op = &in->rhs;
      } else if ((size_t)(k - 3) < in->argument_count) {
        op = &in->arguments[k - 3];
      } else {
        break;
      }
      if (in->op == IR_OP_ADDRESS_OF && op == &in->lhs) {
        continue; /* ADDRESS_OF lowers through MIR_LEA_*; no value preload. */
      }
      if (op->kind != IR_OPERAND_SYMBOL || !op->name ||
          mir_name_map_has(&map, op->name) ||
          !mir_name_is_global_scalar(generator, op->name)) {
        continue;
      }
      Symbol *s = symbol_table_lookup(generator->symbol_table, op->name);
      int size = s ? code_generator_binary_resolved_type_scalar_size(s->type) : 0;
      if (size != 1 && size != 2 && size != 4 && size != 8) {
        continue;
      }
      int is_signed =
          code_generator_binary_resolved_type_is_signed_integer(s->type);
      /* A float global must be cached in an XMM vreg so float consumers read it
       * via the XMM path; a GP cache leaves the bits in a GP register the float
       * ops never read (reading an uninitialized xmm instead). */
      int fbits = code_generator_binary_resolved_type_float_bits(s->type);
      MirVregId v = mir_name_map_get_or_add(
          &map, &fn, op->name, 0, fbits ? MIR_RC_XMM : MIR_RC_GP,
          fbits ? fbits / 8 : 8);
      if (v == MIR_VREG_NONE) {
        goto oom;
      }
      if (!mir_emit1(&fn, MIR_LOAD_GLOBAL, mir_op_vreg(v),
                     mir_op_symbol(op->name), mir_op_none(), size,
                     is_signed ? 0 : 1, 0)) {
        goto oom;
      }
      /* Record this cached global for reload-after-call. The map-has guard
       * above means each global is loaded (and recorded) exactly once. */
      if (wb.all_count >= wb_all_cap) {
        size_t nc = wb_all_cap ? wb_all_cap * 2 : 4;
        const char **grown = (const char **)realloc(wb.all, nc * sizeof(*grown));
        if (!grown) {
          goto oom;
        }
        wb.all = grown;
        wb_all_cap = nc;
      }
      wb.all[wb.all_count++] = op->name;
    }
  }

  /* Address-taken globals (&g): a pointer can read/write their memory, so the
   * cache vreg must be flushed before a pointer LOAD/STORE and reloaded after a
   * pointer STORE. Collect them once (deduped). They are a subset of the cached
   * globals above, so reads/writes still hit the fast register cache between
   * pointer accesses. */
  for (size_t i = 0; i < ir_function->instruction_count; i++) {
    const IRInstruction *in = &ir_function->instructions[i];
    if (in->op != IR_OP_ADDRESS_OF || in->lhs.kind != IR_OPERAND_SYMBOL ||
        !in->lhs.name || !mir_name_is_global_scalar(generator, in->lhs.name)) {
      continue;
    }
    int present = 0;
    for (size_t j = 0; j < wb.at_count; j++) {
      if (strcmp(wb.at[j], in->lhs.name) == 0) {
        present = 1;
        break;
      }
    }
    if (present) {
      continue;
    }
    if (wb.at_count >= wb_at_cap) {
      size_t nc = wb_at_cap ? wb_at_cap * 2 : 4;
      const char **grown = (const char **)realloc(wb.at, nc * sizeof(*grown));
      if (!grown) {
        goto oom;
      }
      wb.at = grown;
      wb_at_cap = nc;
    }
    wb.at[wb.at_count++] = in->lhs.name;
  }

  /* Hoist loop-invariant constants into pooled vregs. Their materialization
   * starts here and is relocated to hot-loop preheaders after MIR layout. */
  if (!mir_build_const_pool(&fn, generator, context, ir_function)) {
    goto oom;
  }

  /* Detect [base + index*scale] address folds before lowering: the producers
   * are marked to skip and each access carries its SIB descriptor. */
  char *fold_skip = NULL;
  MirAddrFold *folds = NULL;
  if (ir_function->instruction_count > 0) {
    fold_skip = (char *)calloc(ir_function->instruction_count, sizeof(char));
    folds = (MirAddrFold *)calloc(ir_function->instruction_count,
                                  sizeof(MirAddrFold));
    if (!fold_skip || !folds) {
      free(fold_skip);
      free(folds);
      goto oom;
    }
    mir_compute_address_folds(ir_function, fold_skip, folds);
    mir_compute_const_compare_skips(generator, context, ir_function, fold_skip);
  }

  for (size_t i = 0; i < ir_function->instruction_count; i++) {
    /* --annotate-asm: attribute every op emitted while lowering this IR
     * instruction back to it (inert when the annotator is off). */
    fn.cur_ir_index = (int)i;
    if (fold_skip[i]) {
      continue; /* address sub-expression folded into a SIB access */
    }
    /* A pointer LOAD/STORE may alias an address-taken global: flush the cached
     * address-taken globals to memory first (so the access sees pending by-name
     * writes), and reload them after a STORE (so a later by-name read sees what
     * the store wrote through the alias). Empty set => no overhead. */
    int mem_op = ir_function->instructions[i].op == IR_OP_LOAD ||
                 ir_function->instructions[i].op == IR_OP_STORE;
    int store_op = ir_function->instructions[i].op == IR_OP_STORE;
    if (mem_op && wb.at_count > 0 &&
        !mir_emit_global_flush_names(&fn, generator, &map, wb.at, wb.at_count)) {
      free(fold_skip);
      free(folds);
      goto oom;
    }
    if (folds[i].valid) {
      if (!mir_lower_folded_access(&fn, generator, context, &map,
                                   &ir_function->instructions[i], &folds[i])) {
        free(fold_skip);
        free(folds);
        goto oom;
      }
    } else if (mir_fuses_compare_branch(generator, ir_function, i)) {
      if (!mir_lower_compare_branch(&fn, generator, context, &map, ir_function,
                                    &ir_function->instructions[i],
                                    &ir_function->instructions[i + 1])) {
        free(fold_skip);
        free(folds);
        goto oom;
      }
      i++; /* consumed the branch_zero too */
    } else {
      /* Around a call, memory is the source of truth for cached globals: flush
       * the written ones first (the callee may read them), lower the call, then
       * reload cached globals only when the callee may have written them. */
      const IRInstruction *cin = &ir_function->instructions[i];
      int is_call = cin->op == IR_OP_CALL || cin->op == IR_OP_CALL_INDIRECT;
      int call_writes_globals =
          is_call ? mir_call_may_write_globals(generator, ir_function, i, cin)
                  : 0;
      if (is_call && wb.all_count > 0 &&
          !mir_emit_global_writebacks(&fn, generator, &map, &wb)) {
        free(fold_skip);
        free(folds);
        goto oom;
      }
      if (!mir_lower_instruction(&fn, generator, context, &map,
                                 &ir_function->instructions[i], &wb)) {
        free(fold_skip);
        free(folds);
        goto oom;
      }
      if (is_call && call_writes_globals && wb.all_count > 0) {
        /* If the call's result is assigned straight to a global (@g = f()), the
         * call lowering already captured RAX into @g's cache vreg; don't reload
         * @g from its stale memory (that would drop the just-stored result). */
        const IROperand *cd = &cin->dest;
        const char *except =
            (cd->kind == IR_OPERAND_SYMBOL && cd->name &&
             mir_name_is_global_scalar(generator, cd->name))
                ? cd->name
                : NULL;
        if (!mir_emit_global_reloads_except(&fn, generator, &map, &wb, except)) {
          free(fold_skip);
          free(folds);
          goto oom;
        }
      }
      /* Keep narrow integer homes canonical: an ASSIGN/BINARY/UNARY/CALL
       * result written to an int32/uint32/int16/etc. variable was computed in
       * 64 bits and may carry garbage above the type's width. LOAD already
       * extends at the access width, CAST canonicalizes itself, and an
       * in-range literal is canonical as materialized, so those are skipped. */
      {
        if (cin->op == IR_OP_ASSIGN || cin->op == IR_OP_BINARY ||
            cin->op == IR_OP_UNARY || cin->op == IR_OP_CALL ||
            cin->op == IR_OP_CALL_INDIRECT) {
          int signed_home = 0;
          int cw =
              mir_dest_integer_narrow_width(generator, context, &cin->dest,
                                            &signed_home);
          int literal_canonical = 0;
          if (cw && cin->op == IR_OP_ASSIGN &&
              cin->lhs.kind == IR_OPERAND_INT) {
            int bits = cw * 8;
            if (signed_home) {
              int64_t minv = -(1ll << (bits - 1));
              int64_t maxv = (1ll << (bits - 1)) - 1;
              literal_canonical = cin->lhs.int_value >= minv &&
                                  cin->lhs.int_value <= maxv;
            } else {
              literal_canonical = cin->lhs.int_value >= 0 &&
                                  (uint64_t)cin->lhs.int_value <
                                      (1ull << bits);
            }
          }
          if (cw && !literal_canonical && !fn.has_error) {
            MirOperand cd =
                mir_value_operand(&fn, generator, context, &map, &cin->dest);
            if (cd.kind == MIR_OPK_VREG &&
                !mir_emit1(&fn, signed_home ? MIR_MOVSX : MIR_MOVZX, cd, cd,
                           mir_op_none(), cw, !signed_home, 0)) {
              free(fold_skip);
              free(folds);
              goto oom;
            }
          }
        }
      }
    }
    if (store_op && wb.at_count > 0 &&
        !mir_emit_global_reload_names(&fn, generator, &map, wb.at,
                                      wb.at_count)) {
      free(fold_skip);
      free(folds);
      goto oom;
    }
    if (fn.has_error) {
      free(fold_skip);
      free(folds);
      goto oom;
    }
  }
  free(fold_skip);
  free(folds);

  mir_fuse_mov_then_extend(&fn);
  mir_rotate_loops(&fn);
  mir_sink_cold_exits(&fn);
  mir_place_const_pool(&fn);

  if (!mir_regalloc(&fn) || fn.has_error) {
    goto oom;
  }
  {
    static int dump = -1;
    if (dump < 0) {
      dump = getenv("METTLE_MIR_DUMP") ? 1 : 0;
    }
    if (dump) {
      mir_function_dump(&fn, stderr);
    }
  }
  /* --annotate-asm: open a capture context so mir_encode's per-instruction
   * records land under this function (inert when the annotator is off). */
  fn.cur_ir_index = -1;
  if (mir_annotate_enabled()) {
    mir_annotate_begin_function(
        function_data->name, ir_function, mir_function_filename(function_data),
        (function_data->body ? function_data->body->location.line : 0));
    mir_annotate_note_backend("register-allocated", NULL);
  }
  if (!mir_encode(&fn) || fn.has_error) {
    mir_annotate_end_function();
    goto oom;
  }
  mir_annotate_end_function();

  free(wb.names);
  free(wb.all);
  free(wb.at);
  mir_name_map_destroy(&map);
  mir_function_destroy(&fn);
  return 1;

oom:
  if (!generator->has_error) {
    code_generator_set_error(generator,
                             "Out of memory or unsupported construct while "
                             "emitting MIR for function '%s'",
                             function_data->name ? function_data->name : "?");
  }
  free(wb.names);
  free(wb.all);
  free(wb.at);
  mir_name_map_destroy(&map);
  mir_function_destroy(&fn);
  return 0;
}
