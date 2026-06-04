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
    fprintf(stderr, "MIR-BAIL\t%s\n", reason);
    (void)fd;
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
static int mir_call_is_supported(CodeGenerator *g, const IRInstruction *in) {
  if (!in->text || in->text[0] == '\0' || in->argument_count > 4) {
    return 0;
  }
  Symbol *callee =
      g->symbol_table ? symbol_table_lookup(g->symbol_table, in->text) : NULL;
  if (!callee || callee->kind != SYMBOL_FUNCTION) {
    return 0;
  }
  Type *ret = callee->data.function.return_type
                  ? callee->data.function.return_type
                  : callee->type;
  if (ret && code_generator_abi_classify(ret) == ABI_PASS_INDIRECT) {
    return 0; /* struct-by-value return uses a hidden pointer: not yet */
  }
  if (callee->data.function.parameter_count != in->argument_count) {
    return 0; /* variadic / arity mismatch: not yet */
  }
  for (size_t a = 0; a < in->argument_count; a++) {
    Type *pt = callee->data.function.parameter_types
                   ? callee->data.function.parameter_types[a]
                   : NULL;
    if (!pt || code_generator_binary_resolved_type_float_bits(pt) != 0 ||
        code_generator_abi_classify(pt) == ABI_PASS_INDIRECT) {
      return 0; /* float arg or struct-by-value arg: deferred */
    }
    const IROperand *arg = &in->arguments[a];
    if (arg->kind != IR_OPERAND_TEMP && arg->kind != IR_OPERAND_SYMBOL &&
        arg->kind != IR_OPERAND_INT) {
      return 0;
    }
  }
  if (in->dest.kind != IR_OPERAND_NONE && in->dest.kind != IR_OPERAND_TEMP &&
      in->dest.kind != IR_OPERAND_SYMBOL) {
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
    return mir_trace_bail(function_data, "sig:params>4");
  }
  for (size_t i = 0; i < function_data->parameter_count; i++) {
    const char *pt =
        function_data->parameter_types ? function_data->parameter_types[i] : NULL;
    if (!mir_type_is_numeric_scalar(generator, pt)) {
      return mir_trace_bail(function_data, "sig:param_nonscalar");
    }
  }
  if (function_data->return_type && function_data->return_type[0] &&
      strcmp(function_data->return_type, "void") != 0 &&
      !mir_type_is_numeric_scalar(generator, function_data->return_type)) {
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
    switch (in->op) {
    case IR_OP_NOP:
    case IR_OP_LABEL:
    case IR_OP_JUMP:
      break;
    case IR_OP_DECLARE_LOCAL:
      if (in->text && !mir_type_is_numeric_scalar(generator, in->text)) {
        return 0;
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
                 !mir_is_comparison(in->text)) {
        return mir_trace_bail(function_data, "binary:divmod_or_other");
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
      break;
    case IR_OP_CALL:
      if (!mir_call_is_supported(generator, in)) {
        return mir_trace_bail(function_data, "call_unsupported");
      }
      break;
    default: {
      /* ADDRESS_OF, UNARY, NEW, CALL_INDIRECT, SIMD ops, ROTATE_ADD: not yet. */
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

/* Emit a MIR_STORE_GLOBAL for each promoted global, writing its cached vreg back
 * to memory. Called immediately before each MIR_RET so memory is consistent on
 * every exit path. */
static int mir_emit_global_writebacks(MirFunction *fn, CodeGenerator *g,
                                      MirNameMap *map,
                                      const MirGlobalWriteback *wb) {
  if (!wb) {
    return 1;
  }
  for (size_t i = 0; i < wb->count; i++) {
    const char *name = wb->names[i];
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

/* Reload every cached global from memory into its cache vreg. Emitted right
 * after a MIR_CALL: the callee may have written any global, so the cached
 * registers are stale. (The written set was flushed before the call via
 * mir_emit_global_writebacks, so memory was current going in.) */
static int mir_emit_global_reloads(MirFunction *fn, CodeGenerator *g,
                                   MirNameMap *map,
                                   const MirGlobalWriteback *wb) {
  if (!wb) {
    return 1;
  }
  for (size_t i = 0; i < wb->all_count; i++) {
    const char *name = wb->all[i];
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
  return mir_emit1(fn, MIR_CMPBR, mir_op_label(br->text), a, b, 8, uns, cc);
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
      return mir_emit1(fn, MIR_SETCC, dst, a, b, 8, uns, cc);
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
    /* Re-express a's 64-bit value as the dst integer type: truncate to dw bytes
     * then extend per dst signedness. dw==8 is a plain move. */
    if (dw == 8) {
      return mir_emit1(fn, MIR_MOV, dst, a, mir_op_none(), 8, 0, 0);
    }
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
    if (in->lhs.kind != IR_OPERAND_NONE) {
      MirOperand src = mir_value_operand(fn, g, ctx, map, &in->lhs);
      int rfb = code_generator_binary_operand_float_bits(g, ctx, &in->lhs);
      if (rfb) {
        /* Float return value goes in XMM0. */
        if (!mir_emit_fmov(fn, mir_op_phys(BINARY_XMM0, MIR_RC_XMM), src,
                           rfb / 8)) {
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
    /* Declare external callees so the linker resolves the relocation. */
    IRFunction *target = code_generator_find_ir_function_binary(g, in->text);
    if (!target) {
      const char *link = code_generator_get_link_symbol_name(g, in->text);
      if (link && !code_generator_binary_declare_external_symbol(g, link)) {
        fn->has_error = 1;
        return 0;
      }
    }
    /* Marshal GP arguments into the ABI's positional argument registers. With
     * all-GP args the positional index equals the argument index on both
     * conventions, and the target registers are never allocatable, so the moves
     * cannot clobber one another. */
    const BinaryAbi *abi = code_generator_binary_active_abi();
    for (size_t a = 0; a < in->argument_count; a++) {
      if (a >= abi->int_param_count) {
        fn->has_error = 1;
        return 0;
      }
      MirOperand arg = mir_value_operand(fn, g, ctx, map, &in->arguments[a]);
      BinaryGpRegister reg = abi->int_param_registers[a];
      if (!mir_emit1(fn, MIR_MOV, mir_op_phys(reg, MIR_RC_GP), arg,
                     mir_op_none(), 8, 0, 0)) {
        return 0;
      }
    }
    if (!mir_emit1(fn, MIR_CALL, mir_op_symbol(in->text), mir_op_none(),
                   mir_op_none(), 8, 0, 0)) {
      return 0;
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
  MirOperand idxo = mir_value_operand(fn, g, ctx, map, &fold->index);
  if (baseo.kind != MIR_OPK_VREG || idxo.kind != MIR_OPK_VREG) {
    fn->has_error = 1;
    return 0;
  }
  MirOperand mem = mir_op_mem_vreg(baseo.vreg, idxo.vreg, fold->scale, 0);
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
    int w = pfb ? pfb / 8 : (pt ? code_generator_binary_resolved_type_scalar_size(pt) : 8);
    if (!pfb && w != 1 && w != 2 && w != 4 && w != 8) {
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
        pt ? code_generator_binary_resolved_type_is_signed_integer(pt) : 1;
    fn.param_count++;
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
  mir_name_map_destroy(&map);
  mir_function_destroy(&fn);
  return 0;
}
