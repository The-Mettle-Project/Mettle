#include "codegen/binary/mir.h"

#include "semantic/symbol_table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* TEMPORARY instrumentation: with METTLE_MIR_TRACE set, log why a function is
 * rejected by the MIR eligibility gate, so the spill-everything-fallback work
 * list can be prioritized by real frequency. Returns 0 (ineligible). */
static int mir_trace_bail(FunctionDeclaration *fd, const char *reason) {
  if (getenv("METTLE_MIR_TRACE")) {
    fprintf(stderr, "MIR-BAIL\t%s\t%s\n", reason,
            (fd && fd->name) ? fd->name : "?");
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
  if (!s || !s->scope || s->scope->type != SCOPE_GLOBAL) {
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
  MirVregId vreg;
} MirNameEntry;

typedef struct {
  MirNameEntry *items;
  size_t count;
  size_t capacity;
} MirNameMap;

static void mir_name_map_destroy(MirNameMap *m) {
  free(m->items);
  m->items = NULL;
  m->count = m->capacity = 0;
}

static MirVregId mir_name_map_get_or_add(MirNameMap *m, MirFunction *fn,
                                         const char *name, MirRegClass rclass,
                                         int width) {
  for (size_t i = 0; i < m->count; i++) {
    if (strcmp(m->items[i].name, name) == 0) {
      return m->items[i].vreg;
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
  m->items[m->count].vreg = v;
  m->count++;
  return v;
}

/* True if `name` already has a vreg binding (param/local/cached global). */
static int mir_name_map_has(const MirNameMap *m, const char *name) {
  for (size_t i = 0; i < m->count; i++) {
    if (strcmp(m->items[i].name, name) == 0) {
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
    MirVregId v = mir_name_map_get_or_add(map, fn, op->name, rc, w);
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
    return 0;
  }
  return 0;
}

/* A direct call MIR can lower: a known function, <=4 register arguments all of
 * GP-scalar type (float args are deferred), a non-INDIRECT (register) return,
 * and simple argument/destination operands. */
static void mir_call_trace(const char *sub) {
  if (getenv("METTLE_MIR_TRACE")) {
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

/* Classify an IR_OP_ADDRESS_OF target. */
typedef enum {
  MIR_ADDROF_UNSUPPORTED = 0, /* function/string/other: deferred */
  MIR_ADDROF_LOCAL,           /* scalar/DIRECT-agg local or parameter (lea home) */
  MIR_ADDROF_GLOBAL,          /* scalar global (lea RIP-relative) */
  MIR_ADDROF_INDIRECT_PARAM   /* INDIRECT-aggregate param: &@p IS the by-ref
                                 pointer, so copy the param value (no home) */
} MirAddrofKind;

static MirAddrofKind mir_addressof_kind(CodeGenerator *g,
                                        const IRFunction *ir_function,
                                        const IRInstruction *in) {
  if (in->lhs.kind != IR_OPERAND_SYMBOL || !in->lhs.name) {
    return MIR_ADDROF_UNSUPPORTED;
  }
  if (code_generator_find_ir_function_binary(g, in->lhs.name)) {
    return MIR_ADDROF_UNSUPPORTED; /* function address */
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
  if (ret && code_generator_abi_classify(ret) == ABI_PASS_INDIRECT) {
    /* struct-by-value return: the caller passes a hidden out-pointer as the
     * first integer arg, pointed at the destination struct's home (a struct
     * LOCAL or a struct TEMP), so the callee writes the result directly there. */
    if (mir_operand_struct_home_size(g, ir_function, &in->dest) == 0) {
      mir_call_trace("ret_indirect");
      return 0;
    }
  }
  if (callee->data.function.parameter_count != in->argument_count) {
    mir_call_trace("arity_mismatch");
    return 0; /* variadic / arity mismatch: not yet */
  }
  for (size_t a = 0; a < in->argument_count; a++) {
    Type *pt = callee->data.function.parameter_types
                   ? callee->data.function.parameter_types[a]
                   : NULL;
    const IROperand *arg = &in->arguments[a];
    if (!pt || code_generator_binary_resolved_type_float_bits(pt) != 0) {
      mir_call_trace("arg_float");
      return 0; /* float arg: deferred (no float arg homing yet) */
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
  /* Kill switch for bisecting MIR vs legacy regressions. */
  {
    const char *off = getenv("METTLE_MIR");
    if (off && off[0] == '0') {
      return 0;
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
                              function_data->parameter_names[i], MIR_RC_GP, 8);
    }
  }
  for (size_t i = 0; i < ir_function->instruction_count; i++) {
    const IRInstruction *in = &ir_function->instructions[i];
    if (in->op == IR_OP_DECLARE_LOCAL && in->dest.kind == IR_OPERAND_SYMBOL &&
        in->dest.name) {
      mir_name_map_get_or_add(&defined, &scratch_fn, in->dest.name, MIR_RC_GP, 8);
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
      /* Integer unary `-`, `~`, `+`, `!` only. Float unary (negate/plus on
       * xmm) and popcnt are deferred to the fallback for now. */
      if (in->is_float || !in->text ||
          (strcmp(in->text, "-") != 0 && strcmp(in->text, "~") != 0 &&
           strcmp(in->text, "+") != 0 && strcmp(in->text, "!") != 0)) {
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
      if (in->lhs.kind != IR_OPERAND_TEMP && in->lhs.kind != IR_OPERAND_SYMBOL) {
        return 0; /* address must be a register-resident pointer */
      }
      if (in->dest.kind != IR_OPERAND_TEMP && in->dest.kind != IR_OPERAND_SYMBOL) {
        return 0;
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
    default: {
      /* UNARY, NEW, CALL_INDIRECT, SIMD ops, ROTATE_ADD: not yet. */
      char buf[40];
      snprintf(buf, sizeof(buf), "op:%d", (int)in->op);
      return mir_trace_bail(function_data, buf);
    }
    }
  }
  if (getenv("METTLE_MIR_TRACE")) {
    fprintf(stderr, "MIR-OK\teligible\n");
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
    MirVregId v = mir_name_map_get_or_add(map, fn, name, MIR_RC_GP, 8);
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
    MirVregId v = mir_name_map_get_or_add(map, fn, name, MIR_RC_GP, 8);
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

/* Reload every cached global from memory into its cache vreg. Emitted after a
 * MIR_CALL: the callee may have written any global, so the cached registers are
 * stale. (The written set was flushed before the call, so memory was current.) */
static int mir_emit_global_reloads(MirFunction *fn, CodeGenerator *g,
                                   MirNameMap *map,
                                   const MirGlobalWriteback *wb) {
  if (!wb) {
    return 1;
  }
  return mir_emit_global_reload_names(fn, g, map, wb->all, wb->all_count);
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

/* Add (bits,width) to the float-constant pool and materialize it once (at the
 * current end of the instruction stream — called before the body is lowered, so
 * it lands at function entry). No-op if already pooled. */
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

/* Build the float-constant pool: every distinct float literal used INSIDE a
 * loop (a backward jump/branch range) is hoisted to a vreg materialized once at
 * entry. Constants outside loops are left inline (no register pressure benefit).
 * Must run before the body is lowered so the materializations land first. */
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
    /* Integer unary only (float unary is gated out in eligibility). */
    MirOperand dst = mir_value_operand(fn, g, ctx, map, &in->dest);
    MirOperand a = mir_value_operand(fn, g, ctx, map, &in->lhs);
    const char *op = in->text ? in->text : "";
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
    int sign_ext = code_generator_binary_load_needs_sign_extend(g, ctx,
                                                               &in->dest, size);
    return mir_emit1(fn, MIR_MOV, dst, mem, mir_op_none(), size,
                     sign_ext ? 0 : 1, 0);
  }

  case IR_OP_STORE: {
    MirOperand addr = mir_value_operand(fn, g, ctx, map, &in->dest);
    MirOperand val = mir_value_operand(fn, g, ctx, map, &in->lhs);
    int size = code_generator_binary_get_access_size(g, ctx, &in->rhs);
    if (size <= 0) {
      fn->has_error = 1;
      return 0;
    }
    MirOperand mem = mir_op_mem_vreg(addr.vreg, MIR_VREG_NONE, 1, 0);
    if (in->is_float) {
      /* A literal value must be materialized at the store width. */
      MirOperand fval = (in->lhs.kind == IR_OPERAND_FLOAT)
                            ? mir_op_fimm(mir_float_bits_at(in->lhs.float_value,
                                                            size))
                            : val;
      return mir_emit_fmov(fn, mem, fval, size);
    }
    return mir_emit1(fn, MIR_MOV, mem, val, mir_op_none(), size, 0, 0);
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
                                fn->indirect_return_size) ||
          !mir_emit1(fn, MIR_MOV, mir_op_phys(BINARY_GP_RAX, MIR_RC_GP),
                     mir_op_vreg(fn->indirect_return_vreg), mir_op_none(), 8, 0,
                     0)) {
        return 0;
      }
      if (!mir_emit_global_writebacks(fn, g, map, wb)) {
        return 0;
      }
      return mir_emit1(fn, MIR_RET, mir_op_none(), mir_op_none(), mir_op_none(),
                       8, 0, 0);
    }
    if (in->lhs.kind != IR_OPERAND_NONE) {
      MirOperand src = mir_value_operand(fn, g, ctx, map, &in->lhs);
      int rfb = code_generator_binary_operand_float_bits(g, ctx, &in->lhs);
      if (rfb) {
        /* Float return value goes in XMM0. */
        if (!mir_emit_fmov(fn, mir_op_phys(BINARY_XMM0, MIR_RC_XMM), src,
                           rfb / 8)) {
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
    /* Flush register-promoted globals to memory before returning. */
    if (!mir_emit_global_writebacks(fn, g, map, wb)) {
      return 0;
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
    /* Marshal GP arguments per the ABI layout. Arguments up to the ABI's
     * argument-register count go into registers; the rest are stored into the
     * outgoing stack-argument region (reserved once in the prologue). All call
     * args are GP here (float/struct args bail in eligibility). */
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
    int sign_ext =
        code_generator_binary_load_needs_sign_extend(g, ctx, &in->dest, size);
    return mir_emit1(fn, MIR_MOV, dst, mem, mir_op_none(), size,
                     sign_ext ? 0 : 1, 0);
  }
  MirOperand val = mir_value_operand(fn, g, ctx, map, &in->lhs);
  return mir_emit1(fn, MIR_MOV, mem, val, mir_op_none(), size, 0, 0);
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
    MirVregId v = mir_name_map_get_or_add(&map, &fn, pname,
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
    /* Load each global (read or written) into its cache vreg once at entry. */
    const IROperand *ops[3] = {&in->dest, &in->lhs, &in->rhs};
    for (int k = 0; k < 3; k++) {
      const IROperand *op = ops[k];
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
      MirVregId v =
          mir_name_map_get_or_add(&map, &fn, op->name, MIR_RC_GP, 8);
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

  /* Hoist loop-invariant float constants (materializes them at entry, so this
   * must precede body lowering). */
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
       * reload every cached global (the callee may have written any of them). */
      int is_call = ir_function->instructions[i].op == IR_OP_CALL;
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
      if (is_call && wb.all_count > 0 &&
          !mir_emit_global_reloads(&fn, generator, &map, &wb)) {
        free(fold_skip);
        free(folds);
        goto oom;
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

  if (!mir_regalloc(&fn) || fn.has_error) {
    goto oom;
  }
  if (getenv("METTLE_MIR_DUMP")) {
    mir_function_dump(&fn, stderr);
  }
  if (!mir_encode(&fn) || fn.has_error) {
    goto oom;
  }

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
