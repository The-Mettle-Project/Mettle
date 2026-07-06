#include "ir_optimize_internal.h"

/* ---- Allocation-site layout factorization ----------------------------------
 *
 * A heap pool allocated as `malloc(N*S)` and accessed only through the affine
 * family `base + i*S + c` (constant stride S, constant field offsets c) is a
 * hand-rolled array-of-structs. When the whole program's uses of that pointer
 * are provably confined to that family, the interior layout of the allocation
 * is unobservable and can be re-mapped:
 *
 *   COMPACT: repack the field offsets to a denser stride S' = sum of field
 *            widths, shrinking the working set when the source stride carried
 *            padding (16-byte nodes with 8 live bytes -> 8-byte nodes).
 *   SOA:     partition the SAME allocation into one contiguous array per
 *            field (base + P_k + i*w_k). Traversals that read a subset of the
 *            fields stop pulling the cold fields through the cache, and every
 *            field array becomes unit-stride for the vectorizers.
 *
 * Both rewrites stay inside the original allocation (SOA keeps partition 0 at
 * byte 0 and sum(n*w_k) <= n*S always holds), so free(base) and null checks
 * are untouched.
 *
 * Eligibility is a whole-program pointer-use analysis in the spirit of SROA:
 * the base pointer is tracked through copies, casts, and call arguments into
 * program-defined callees; every transitive use must be an affine deref, a
 * compare against 0, free(base), or a tracked call. Any other use (escape
 * into memory, extern call, return, arithmetic outside the family, mixed
 * strides, overlapping fields) disqualifies the allocation. Callees whose
 * bodies get rewritten must receive this pool at every call site in the
 * program, which is only knowable in a whole-program (executable) build.
 *
 * This pass runs OUTSIDE per-function --verify validation by design: it
 * preserves program-observable behavior but changes the buffer's byte image,
 * which the per-function validator treats as an observation (and a
 * multi-function rewrite must not be quarantined per function, which would
 * mix layouts). METTLE_SKIP_PASS=layout_factor disables it. */

#define IR_LAYOUT_MAX_CLASSES 12
#define IR_LAYOUT_MAX_ALIASES 256
#define IR_LAYOUT_MAX_DERIVED 4096
#define IR_LAYOUT_MAX_DEREFS 4096
#define IR_LAYOUT_MAX_FUNCS 16
#define IR_LAYOUT_MAX_CALL_EDGES 32
#define IR_LAYOUT_RESOLVE_DEPTH 12

typedef struct {
  long long offset;
  int width;
  long long new_offset; /* COMPACT: offset within S'; SOA: partition base P_k */
  int seen_in_load;
} IRLayoutClass;

typedef struct {
  IRFunction *fn;
  const char *name;
  IROperandKind kind;
} IRLayoutAlias;

/* A temp holding base + (i*S)? + c inside one function. */
typedef struct {
  IRFunction *fn;
  const char *temp;
  const char *base_name;
  IROperandKind base_kind;
  int has_index;
  IROperand index; /* the operand the original code scaled by S (borrowed) */
  long long offset;
} IRLayoutDerived;

typedef struct {
  IRFunction *fn;
  size_t insn_index;
  int is_store;
  long long offset;
  int width;
  int has_index;
  IROperand index;     /* borrowed from the derived record */
  const char *base_name;
  IROperandKind base_kind;
  size_t class_index;
} IRLayoutDeref;

typedef struct {
  IRFunction *callee;
  size_t param_index;
} IRLayoutCallEdge;

typedef struct {
  IRProgram *program;
  IRFunction *alloc_fn;
  size_t alloc_index;
  long long alloc_size;
  long long stride; /* S; 0 until the first indexed deref pins it */

  IRLayoutAlias aliases[IR_LAYOUT_MAX_ALIASES];
  size_t alias_count;
  IRLayoutDerived derived[IR_LAYOUT_MAX_DERIVED];
  size_t derived_count;
  IRLayoutDeref derefs[IR_LAYOUT_MAX_DEREFS];
  size_t deref_count;
  IRLayoutClass classes[IR_LAYOUT_MAX_CLASSES];
  size_t class_count;
  IRFunction *funcs[IR_LAYOUT_MAX_FUNCS];
  size_t func_count;
  IRLayoutCallEdge edges[IR_LAYOUT_MAX_CALL_EDGES];
  size_t edge_count;
} IRLayoutCandidate;

static size_t g_layout_temp_id;

/* METTLE_LAYOUT_DEBUG=1: trace candidate decisions to stderr. */
static int ir_layout_debug_enabled(void) {
  static int cached = -1;
  if (cached < 0) {
    const char *env = getenv("METTLE_LAYOUT_DEBUG");
    cached = env && *env && strcmp(env, "0") != 0;
  }
  return cached;
}

static void ir_layout_debug(const char *fmt, const char *a, long long b) {
  if (ir_layout_debug_enabled()) {
    fprintf(stderr, "[layout] ");
    fprintf(stderr, fmt, a ? a : "?", b);
    fprintf(stderr, "\n");
  }
}

static void ir_layout_debug_insn(const char *what, const IRFunction *fn,
                                 const IRInstruction *insn) {
  if (ir_layout_debug_enabled()) {
    char buf[256];
    ir_instruction_dump(insn, buf, sizeof(buf));
    fprintf(stderr, "[layout] %s in %s: %s\n", what,
            fn->name ? fn->name : "?", buf);
  }
}

static int ir_layout_operand_is_alias(const IRLayoutCandidate *cand,
                                      IRFunction *fn, const IROperand *op) {
  if (!op->name ||
      (op->kind != IR_OPERAND_SYMBOL && op->kind != IR_OPERAND_TEMP)) {
    return 0;
  }
  for (size_t i = 0; i < cand->alias_count; i++) {
    if (cand->aliases[i].fn == fn && cand->aliases[i].kind == op->kind &&
        strcmp(cand->aliases[i].name, op->name) == 0) {
      return 1;
    }
  }
  return 0;
}

static int ir_layout_alias_add(IRLayoutCandidate *cand, IRFunction *fn,
                               const char *name, IROperandKind kind) {
  IROperand probe = {0};
  probe.kind = kind;
  probe.name = (char *)name;
  if (ir_layout_operand_is_alias(cand, fn, &probe)) {
    return 1;
  }
  if (cand->alias_count >= IR_LAYOUT_MAX_ALIASES) {
    return 0;
  }
  cand->aliases[cand->alias_count].fn = fn;
  cand->aliases[cand->alias_count].name = name;
  cand->aliases[cand->alias_count].kind = kind;
  cand->alias_count++;
  return 1;
}

static IRLayoutDerived *ir_layout_find_derived(IRLayoutCandidate *cand,
                                               IRFunction *fn,
                                               const IROperand *op) {
  if (op->kind != IR_OPERAND_TEMP || !op->name) {
    return NULL;
  }
  for (size_t i = 0; i < cand->derived_count; i++) {
    if (cand->derived[i].fn == fn &&
        strcmp(cand->derived[i].temp, op->name) == 0) {
      return &cand->derived[i];
    }
  }
  return NULL;
}

/* Whole-function symbol constant: exactly one write anywhere in `fn`, and it
 * is `ASSIGN sym <- INT`, with the address never taken and the name not a
 * parameter. Covers locals initialized once from a folded read-only global
 * when the block-local symbol map has been invalidated by earlier control
 * flow. */
static int ir_layout_symbol_single_const(IRFunction *fn, const char *name,
                                         long long *out) {
  if (!name || ir_function_symbol_is_parameter(fn, name)) {
    return 0;
  }
  const IRInstruction *write = NULL;
  for (size_t i = 0; i < fn->instruction_count; i++) {
    const IRInstruction *insn = &fn->instructions[i];
    if (insn->op == IR_OP_ADDRESS_OF &&
        ir_operand_is_symbol_named(&insn->lhs, name)) {
      return 0;
    }
    if (insn->op == IR_OP_DECLARE_LOCAL) {
      continue;
    }
    if (ir_instruction_writes_destination(insn) &&
        ir_operand_is_symbol_named(&insn->dest, name)) {
      if (write) {
        return 0;
      }
      write = insn;
    }
  }
  if (!write || write->op != IR_OP_ASSIGN ||
      write->lhs.kind != IR_OPERAND_INT) {
    return 0;
  }
  *out = write->lhs.int_value;
  return 1;
}

/* Resolve `op` (at instruction `at` in `fn`) to a compile-time integer.
 * Handles INT literals, symbols with a known value at `at` (via the caller's
 * prebuilt symbol map, falling back to the whole-function single-write
 * analysis), and temps produced by straight-line CAST/int BINARY chains over
 * resolvable operands. */
static int ir_layout_resolve_const(IRFunction *fn, size_t at,
                                   const IRSymbolValueMap *symbol_map,
                                   const IROperand *op, int depth,
                                   long long *out) {
  if (depth > IR_LAYOUT_RESOLVE_DEPTH) {
    return 0;
  }
  if (op->kind == IR_OPERAND_INT) {
    *out = op->int_value;
    return 1;
  }
  if (op->kind == IR_OPERAND_SYMBOL) {
    if (symbol_map && ir_operand_resolve_symbol_int(symbol_map, op, out)) {
      return 1;
    }
    return ir_layout_symbol_single_const(fn, op->name, out);
  }
  if (op->kind != IR_OPERAND_TEMP || !op->name) {
    return 0;
  }
  const IRInstruction *producer =
      ir_find_temp_producer_before(fn, at, op->name);
  if (!producer) {
    return 0;
  }
  size_t prod_index = (size_t)(producer - fn->instructions);
  if (producer->op == IR_OP_ASSIGN || producer->op == IR_OP_CAST) {
    if (producer->is_float) {
      return 0;
    }
    return ir_layout_resolve_const(fn, prod_index, symbol_map, &producer->lhs,
                                   depth + 1, out);
  }
  if (producer->op == IR_OP_BINARY && producer->text && !producer->is_float) {
    long long a = 0;
    long long b = 0;
    if (!ir_layout_resolve_const(fn, prod_index, symbol_map, &producer->lhs,
                                 depth + 1, &a) ||
        !ir_layout_resolve_const(fn, prod_index, symbol_map, &producer->rhs,
                                 depth + 1, &b)) {
      return 0;
    }
    if (strcmp(producer->text, "*") == 0) {
      *out = a * b;
      return 1;
    }
    if (strcmp(producer->text, "+") == 0) {
      *out = a + b;
      return 1;
    }
    if (strcmp(producer->text, "-") == 0) {
      *out = a - b;
      return 1;
    }
    if (strcmp(producer->text, "<<") == 0 && b >= 0 && b < 63) {
      *out = a << b;
      return 1;
    }
  }
  return 0;
}

/* A pointer-width cast target: copying the pool through it cannot lose bits. */
static int ir_layout_cast_preserves_pointer(const char *type_name) {
  if (!type_name) {
    return 0;
  }
  if (strchr(type_name, '*')) {
    return 1;
  }
  return strcmp(type_name, "cstring") == 0 || strcmp(type_name, "int64") == 0 ||
         strcmp(type_name, "uint64") == 0;
}

/* Match `op` as a scaled index: a temp produced by `X * CONST` or
 * `X << CONST` before `at`. Writes the scale and the unscaled operand. */
static int ir_layout_match_scaled_index(IRFunction *fn, size_t at,
                                        const IROperand *op,
                                        long long *scale_out,
                                        IROperand *index_out) {
  if (op->kind != IR_OPERAND_TEMP || !op->name) {
    return 0;
  }
  const IRInstruction *producer = ir_find_temp_producer_before(fn, at, op->name);
  if (!producer || producer->op != IR_OP_BINARY || !producer->text ||
      producer->is_float) {
    return 0;
  }
  if (strcmp(producer->text, "*") == 0) {
    if (producer->rhs.kind == IR_OPERAND_INT && producer->rhs.int_value > 0) {
      *scale_out = producer->rhs.int_value;
      *index_out = producer->lhs;
      return 1;
    }
    if (producer->lhs.kind == IR_OPERAND_INT && producer->lhs.int_value > 0) {
      *scale_out = producer->lhs.int_value;
      *index_out = producer->rhs;
      return 1;
    }
    return 0;
  }
  if (strcmp(producer->text, "<<") == 0 &&
      producer->rhs.kind == IR_OPERAND_INT && producer->rhs.int_value >= 0 &&
      producer->rhs.int_value < 32) {
    *scale_out = 1LL << producer->rhs.int_value;
    *index_out = producer->lhs;
    return 1;
  }
  return 0;
}

static int ir_layout_note_class(IRLayoutCandidate *cand, long long offset,
                                int width, int is_load) {
  for (size_t i = 0; i < cand->class_count; i++) {
    if (cand->classes[i].offset == offset) {
      if (cand->classes[i].width != width) {
        return 0; /* mixed widths at one offset: not a clean field */
      }
      if (is_load) {
        cand->classes[i].seen_in_load = 1;
      }
      return 1;
    }
  }
  if (cand->class_count >= IR_LAYOUT_MAX_CLASSES) {
    return 0;
  }
  cand->classes[cand->class_count].offset = offset;
  cand->classes[cand->class_count].width = width;
  cand->classes[cand->class_count].new_offset = 0;
  cand->classes[cand->class_count].seen_in_load = is_load ? 1 : 0;
  cand->class_count++;
  return 1;
}

static int ir_layout_note_deref(IRLayoutCandidate *cand, IRFunction *fn,
                                size_t insn_index, int is_store,
                                long long offset, int width, int has_index,
                                const IROperand *index, const char *base_name,
                                IROperandKind base_kind) {
  if (offset < 0 || width <= 0 || cand->deref_count >= IR_LAYOUT_MAX_DEREFS) {
    return 0;
  }
  if (!ir_layout_note_class(cand, offset, width, !is_store)) {
    return 0;
  }
  IRLayoutDeref *d = &cand->derefs[cand->deref_count++];
  d->fn = fn;
  d->insn_index = insn_index;
  d->is_store = is_store;
  d->offset = offset;
  d->width = width;
  d->has_index = has_index;
  if (has_index) {
    d->index = *index;
  } else {
    memset(&d->index, 0, sizeof(d->index));
  }
  d->base_name = base_name;
  d->base_kind = base_kind;
  d->class_index = 0;
  return 1;
}

static int ir_layout_deref_width(const IROperand *size_operand) {
  return size_operand->kind == IR_OPERAND_INT ? (int)size_operand->int_value
                                              : 8;
}

static int ir_layout_func_tracked(const IRLayoutCandidate *cand,
                                  const IRFunction *fn) {
  for (size_t i = 0; i < cand->func_count; i++) {
    if (cand->funcs[i] == fn) {
      return 1;
    }
  }
  return 0;
}

static int ir_layout_edge_add(IRLayoutCandidate *cand, IRFunction *callee,
                              size_t param_index) {
  for (size_t i = 0; i < cand->edge_count; i++) {
    if (cand->edges[i].callee == callee &&
        cand->edges[i].param_index == param_index) {
      return 1;
    }
  }
  if (cand->edge_count >= IR_LAYOUT_MAX_CALL_EDGES) {
    return 0;
  }
  cand->edges[cand->edge_count].callee = callee;
  cand->edges[cand->edge_count].param_index = param_index;
  cand->edge_count++;
  return 1;
}

/* Grow the alias/derived sets for one function until stable. Returns 0 on
 * a shape that disqualifies the candidate (only shapes that create records
 * are judged here; the validation sweep judges every remaining use). */
static int ir_layout_grow_function(IRLayoutCandidate *cand, IRFunction *fn) {
  for (int round = 0; round < 8; round++) {
    int grew = 0;
    for (size_t i = 0; i < fn->instruction_count; i++) {
      const IRInstruction *insn = &fn->instructions[i];

      if ((insn->op == IR_OP_ASSIGN || insn->op == IR_OP_CAST) &&
          ir_layout_operand_is_alias(cand, fn, &insn->lhs) &&
          insn->dest.name &&
          (insn->dest.kind == IR_OPERAND_SYMBOL ||
           insn->dest.kind == IR_OPERAND_TEMP)) {
        if (insn->op == IR_OP_CAST &&
            !ir_layout_cast_preserves_pointer(insn->text)) {
          ir_layout_debug_insn("narrowing cast of pool", fn, insn);
          return 0;
        }
        IROperand probe = insn->dest;
        if (!ir_layout_operand_is_alias(cand, fn, &probe)) {
          if (!ir_layout_alias_add(cand, fn, insn->dest.name,
                                   insn->dest.kind)) {
            ir_layout_debug("alias cap hit in %s (%lld)", fn->name,
                            (long long)cand->alias_count);
            return 0;
          }
          grew = 1;
        }
        continue;
      }

      /* Pointer-type cast of a derived address (`(int32*)(base+i*S+c)` right
       * before the deref): the cast result is the same interior pointer. */
      if (insn->op == IR_OP_CAST && insn->dest.kind == IR_OPERAND_TEMP &&
          insn->dest.name) {
        IRLayoutDerived *src = ir_layout_find_derived(cand, fn, &insn->lhs);
        if (src) {
          if (!ir_layout_cast_preserves_pointer(insn->text)) {
            ir_layout_debug_insn("narrowing cast of derived", fn, insn);
            return 0;
          }
          IROperand probe = insn->dest;
          if (!ir_layout_find_derived(cand, fn, &probe)) {
            if (cand->derived_count >= IR_LAYOUT_MAX_DERIVED) {
              ir_layout_debug("derived cap hit in %s (%lld)", fn->name,
                              (long long)cand->derived_count);
              return 0;
            }
            IRLayoutDerived *d = &cand->derived[cand->derived_count];
            *d = *src;
            d->temp = insn->dest.name;
            cand->derived_count++;
            grew = 1;
          }
          continue;
        }
      }

      if (insn->op != IR_OP_BINARY || !insn->text ||
          strcmp(insn->text, "+") != 0 || insn->is_float ||
          insn->dest.kind != IR_OPERAND_TEMP || !insn->dest.name) {
        continue;
      }
      IROperand probe = insn->dest;
      if (ir_layout_find_derived(cand, fn, &probe)) {
        continue;
      }

      const IROperand *ptr_side = NULL;
      const IROperand *add_side = NULL;
      if (ir_layout_operand_is_alias(cand, fn, &insn->lhs)) {
        ptr_side = &insn->lhs;
        add_side = &insn->rhs;
      } else if (ir_layout_operand_is_alias(cand, fn, &insn->rhs)) {
        ptr_side = &insn->rhs;
        add_side = &insn->lhs;
      }
      if (ptr_side) {
        if (cand->derived_count >= IR_LAYOUT_MAX_DERIVED) {
          ir_layout_debug("derived cap hit in %s (%lld)", fn->name,
                          (long long)cand->derived_count);
          return 0;
        }
        IRLayoutDerived *d = &cand->derived[cand->derived_count];
        d->fn = fn;
        d->temp = insn->dest.name;
        d->base_name = ptr_side->name;
        d->base_kind = ptr_side->kind;
        d->offset = 0;
        d->has_index = 0;
        memset(&d->index, 0, sizeof(d->index));
        if (add_side->kind == IR_OPERAND_INT) {
          d->offset = add_side->int_value;
        } else {
          long long scale = 0;
          IROperand index = {0};
          if (!ir_layout_match_scaled_index(fn, i, add_side, &scale, &index)) {
            ir_layout_debug_insn("non-affine pointer add", fn, insn);
            return 0; /* pointer + unrecognized value: not the affine family */
          }
          if (cand->stride == 0) {
            cand->stride = scale;
          } else if (cand->stride != scale) {
            ir_layout_debug("mixed stride %s: %lld", fn->name, scale);
            return 0; /* mixed strides across the pool's derefs */
          }
          d->has_index = 1;
          d->index = index;
        }
        cand->derived_count++;
        grew = 1;
        continue;
      }

      /* Extension: derived + CONST (either operand order). The offset side may
       * be a temp that resolves to a compile-time constant through its
       * producer chain -- inlining leaves `field*4` unfolded because this
       * pass runs before the const-fold fixpoint. */
      IRLayoutDerived *base = ir_layout_find_derived(cand, fn, &insn->lhs);
      const IROperand *const_side = &insn->rhs;
      if (!base) {
        base = ir_layout_find_derived(cand, fn, &insn->rhs);
        const_side = &insn->lhs;
      }
      if (base) {
        long long extra = 0;
        if (const_side->kind == IR_OPERAND_INT) {
          extra = const_side->int_value;
        } else if (!ir_layout_resolve_const(fn, i, NULL, const_side, 0,
                                            &extra)) {
          ir_layout_debug_insn("derived + non-const", fn, insn);
          return 0;
        }
        if (cand->derived_count >= IR_LAYOUT_MAX_DERIVED) {
          ir_layout_debug("derived cap hit in %s (%lld)", fn->name,
                          (long long)cand->derived_count);
          return 0;
        }
        IRLayoutDerived *d = &cand->derived[cand->derived_count];
        *d = *base;
        d->temp = insn->dest.name;
        d->offset = base->offset + extra;
        cand->derived_count++;
        grew = 1;
      }
    }
    if (!grew) {
      return 1;
    }
  }
  return 1;
}

static int ir_layout_operand_mentions(const IRLayoutCandidate *cand,
                                      IRFunction *fn, const IROperand *op) {
  if (ir_layout_operand_is_alias(cand, fn, op)) {
    return 1;
  }
  IROperand probe = *op;
  return ir_layout_find_derived((IRLayoutCandidate *)cand, fn, &probe) != NULL;
}

/* Validate every use of every alias/derived temp in `fn`, recording derefs
 * and interprocedural edges. Runs only after the alias closure is complete;
 * any use it cannot classify disqualifies the candidate (returns 0). */
static int ir_layout_validate_function(IRLayoutCandidate *cand,
                                       IRFunction *fn) {
  for (size_t i = 0; i < fn->instruction_count; i++) {
    const IRInstruction *insn = &fn->instructions[i];
    if (fn == cand->alloc_fn && i == cand->alloc_index) {
      continue; /* the candidate's own malloc call defines the base */
    }
    int mentions = 0;
    if (ir_layout_operand_mentions(cand, fn, &insn->dest) ||
        ir_layout_operand_mentions(cand, fn, &insn->lhs) ||
        ir_layout_operand_mentions(cand, fn, &insn->rhs)) {
      mentions = 1;
    }
    for (size_t a = 0; a < insn->argument_count && !mentions; a++) {
      if (ir_layout_operand_mentions(cand, fn, &insn->arguments[a])) {
        mentions = 1;
      }
    }
    if (!mentions) {
      continue;
    }

    switch (insn->op) {
    case IR_OP_DECLARE_LOCAL:
      continue; /* declaring an alias local carries no value */

    case IR_OP_NOP:
      continue; /* @simd/loop markers reference nothing */

    case IR_OP_ASSIGN:
    case IR_OP_CAST:
      /* Only the recorded alias-copy shape is legal: alias -> alias. An
       * alias DEST being overwritten from a non-alias source would fork the
       * symbol's meaning mid-function; a derived temp on either side
       * escapes the family. */
      if (ir_layout_operand_is_alias(cand, fn, &insn->lhs) &&
          ir_layout_operand_is_alias(cand, fn, &insn->dest) &&
          !ir_layout_find_derived(cand, fn, &insn->lhs) &&
          !ir_layout_find_derived(cand, fn, &insn->dest)) {
        continue;
      }
      /* The recorded derived->derived pointer cast. */
      if (insn->op == IR_OP_CAST &&
          ir_layout_find_derived(cand, fn, &insn->lhs) &&
          insn->dest.kind == IR_OPERAND_TEMP) {
        IROperand probe = insn->dest;
        if (ir_layout_find_derived(cand, fn, &probe)) {
          continue;
        }
      }
      ir_layout_debug_insn("escape (assign/cast)", fn, insn);
      return 0;

    case IR_OP_BINARY: {
      IROperand probe = insn->dest;
      if (insn->dest.kind == IR_OPERAND_TEMP &&
          ir_layout_find_derived(cand, fn, &probe)) {
        continue; /* a recorded derived-pointer producer */
      }
      /* Pointer compare against literal 0 (null checks). */
      if (insn->text &&
          (strcmp(insn->text, "==") == 0 || strcmp(insn->text, "!=") == 0) &&
          !ir_layout_operand_mentions(cand, fn, &insn->dest)) {
        const IROperand *other = ir_layout_operand_is_alias(cand, fn, &insn->lhs)
                                     ? &insn->rhs
                                     : &insn->lhs;
        if (ir_layout_operand_is_alias(cand, fn, other)) {
          return 0;
        }
        if (other->kind == IR_OPERAND_INT && other->int_value == 0 &&
            !ir_layout_find_derived(cand, fn, &insn->lhs) &&
            !ir_layout_find_derived(cand, fn, &insn->rhs)) {
          continue;
        }
      }
      ir_layout_debug_insn("escape (binary)", fn, insn);
      return 0;
    }

    case IR_OP_LOAD: {
      if (ir_layout_operand_mentions(cand, fn, &insn->dest) ||
          ir_layout_operand_mentions(cand, fn, &insn->rhs)) {
        return 0;
      }
      IRLayoutDerived *d = ir_layout_find_derived(cand, fn, &insn->lhs);
      int width = ir_layout_deref_width(&insn->rhs);
      if (d) {
        if (!ir_layout_note_deref(cand, fn, i, 0, d->offset, width,
                                  d->has_index, &d->index, d->base_name,
                                  d->base_kind)) {
          return 0;
        }
        continue;
      }
      if (ir_layout_operand_is_alias(cand, fn, &insn->lhs)) {
        if (!ir_layout_note_deref(cand, fn, i, 0, 0, width, 0, NULL,
                                  insn->lhs.name, insn->lhs.kind)) {
          return 0;
        }
        continue;
      }
      return 0;
    }

    case IR_OP_STORE: {
      if (ir_layout_operand_mentions(cand, fn, &insn->lhs) ||
          ir_layout_operand_mentions(cand, fn, &insn->rhs)) {
        return 0; /* the pool pointer stored as a VALUE escapes */
      }
      IRLayoutDerived *d = ir_layout_find_derived(cand, fn, &insn->dest);
      int width = ir_layout_deref_width(&insn->rhs);
      if (d) {
        if (!ir_layout_note_deref(cand, fn, i, 1, d->offset, width,
                                  d->has_index, &d->index, d->base_name,
                                  d->base_kind)) {
          return 0;
        }
        continue;
      }
      if (ir_layout_operand_is_alias(cand, fn, &insn->dest)) {
        if (!ir_layout_note_deref(cand, fn, i, 1, 0, width, 0, NULL,
                                  insn->dest.name, insn->dest.kind)) {
          return 0;
        }
        continue;
      }
      return 0;
    }

    case IR_OP_CALL: {
      if (ir_layout_operand_mentions(cand, fn, &insn->dest)) {
        return 0;
      }
      if (!insn->text) {
        return 0;
      }
      for (size_t a = 0; a < insn->argument_count; a++) {
        const IROperand *arg = &insn->arguments[a];
        IROperand probe = *arg;
        if (ir_layout_find_derived(cand, fn, &probe)) {
          return 0; /* interior pointer passed out */
        }
        if (!ir_layout_operand_is_alias(cand, fn, arg)) {
          continue;
        }
        if (strcmp(insn->text, "free") == 0 && insn->argument_count == 1) {
          continue; /* whole-block free of the base pointer */
        }
        IRFunction *callee = ir_program_find_function(cand->program, insn->text);
        if (!callee || a >= callee->parameter_count ||
            !callee->parameter_names[a]) {
          return 0; /* extern or shape mismatch */
        }
        if (!ir_layout_edge_add(cand, callee, a)) {
          return 0;
        }
        /* The closure must already know this edge; a fresh discovery here
         * means the fixpoint missed something -- bail, never guess. */
        if (!ir_layout_func_tracked(cand, callee)) {
          return 0;
        }
        IROperand param_probe = {0};
        param_probe.kind = IR_OPERAND_SYMBOL;
        param_probe.name = (char *)callee->parameter_names[a];
        if (!ir_layout_operand_is_alias(cand, callee, &param_probe)) {
          return 0;
        }
      }
      continue;
    }

    default:
      ir_layout_debug_insn("escape (op)", fn, insn);
      return 0; /* RETURN, INLINE_ASM, SIMD kernels, memcpy, ... */
    }
  }
  return 1;
}

/* Every call site of every tracked callee must pass this candidate's pool at
 * the tracked parameter position, and no instruction anywhere may reference a
 * tracked callee as a value (function pointer / dispatch-by-name). */
static int ir_layout_check_call_completeness(IRLayoutCandidate *cand) {
  for (size_t e = 0; e < cand->edge_count; e++) {
    IRFunction *callee = cand->edges[e].callee;
    size_t k = cand->edges[e].param_index;
    for (size_t f = 0; f < cand->program->function_count; f++) {
      IRFunction *fn = cand->program->functions[f];
      for (size_t i = 0; i < fn->instruction_count; i++) {
        const IRInstruction *insn = &fn->instructions[i];
        if (insn->op == IR_OP_CALL && insn->text &&
            strcmp(insn->text, callee->name) == 0) {
          if (!ir_layout_func_tracked(cand, fn) && fn != cand->alloc_fn) {
            return 0;
          }
          if (k >= insn->argument_count ||
              !ir_layout_operand_is_alias(cand, fn, &insn->arguments[k])) {
            return 0;
          }
          continue;
        }
        /* Any non-call reference to the callee's name (function pointers,
         * indirect dispatch) makes its call sites unknowable. */
        const IROperand *ops[3] = {&insn->dest, &insn->lhs, &insn->rhs};
        for (size_t o = 0; o < 3; o++) {
          if ((ops[o]->kind == IR_OPERAND_SYMBOL ||
               ops[o]->kind == IR_OPERAND_STRING) &&
              ops[o]->name && strcmp(ops[o]->name, callee->name) == 0) {
            return 0;
          }
        }
        for (size_t a = 0; a < insn->argument_count; a++) {
          const IROperand *op = &insn->arguments[a];
          if ((op->kind == IR_OPERAND_SYMBOL ||
               op->kind == IR_OPERAND_STRING) &&
              op->name && strcmp(op->name, callee->name) == 0) {
            return 0;
          }
        }
      }
    }
  }
  return 1;
}

static int ir_layout_class_cmp_offset(const void *a, const void *b) {
  const IRLayoutClass *ca = (const IRLayoutClass *)a;
  const IRLayoutClass *cb = (const IRLayoutClass *)b;
  return ca->offset < cb->offset ? -1 : (ca->offset > cb->offset ? 1 : 0);
}

/* Assign new offsets. mode 1 = COMPACT (returns new stride S'), mode 2 = SOA
 * (returns 0; class new_offset holds the partition base P_k). Classes are
 * assigned in descending width order so every offset lands naturally
 * aligned. */
static long long ir_layout_assign_offsets(IRLayoutCandidate *cand, int mode,
                                          long long element_count) {
  size_t order[IR_LAYOUT_MAX_CLASSES];
  for (size_t i = 0; i < cand->class_count; i++) {
    order[i] = i;
  }
  for (size_t i = 0; i < cand->class_count; i++) {
    for (size_t j = i + 1; j < cand->class_count; j++) {
      if (cand->classes[order[j]].width > cand->classes[order[i]].width) {
        size_t t = order[i];
        order[i] = order[j];
        order[j] = t;
      }
    }
  }
  long long cursor = 0;
  int max_width = 1;
  for (size_t i = 0; i < cand->class_count; i++) {
    IRLayoutClass *c = &cand->classes[order[i]];
    c->new_offset = cursor;
    cursor += mode == 1 ? c->width : element_count * c->width;
    if (c->width > max_width) {
      max_width = c->width;
    }
  }
  if (mode == 1) {
    return (cursor + max_width - 1) / max_width * max_width;
  }
  return 0;
}

static char *ir_layout_temp_name(void) {
  char buf[32];
  snprintf(buf, sizeof(buf), ".ly%zu", g_layout_temp_id++);
  return mettle_strdup(buf);
}

static int ir_layout_emit_binary(IRInstructionVector *vec,
                                 SourceLocation location, const char *op_text,
                                 const char *dest_temp, const IROperand *lhs,
                                 const IROperand *rhs) {
  IRInstruction insn = {0};
  insn.op = IR_OP_BINARY;
  insn.location = location;
  insn.text = mettle_strdup(op_text);
  insn.dest = ir_operand_temp(dest_temp);
  if (!insn.text || !insn.dest.name || !ir_operand_clone(lhs, &insn.lhs) ||
      !ir_operand_clone(rhs, &insn.rhs) ||
      !ir_instruction_vector_append_move(vec, &insn)) {
    ir_instruction_destroy_storage(&insn);
    return 0;
  }
  return 1;
}

/* Emit the replacement address chain for one deref and return the final
 * address temp name (owned by the caller). mode/stride per the candidate's
 * decision; per-class new offsets already assigned. */
static char *ir_layout_emit_address(IRInstructionVector *vec,
                                    const IRLayoutDeref *deref,
                                    const IRLayoutClass *cls, int mode,
                                    long long new_stride,
                                    SourceLocation location) {
  IROperand base = {0};
  base.kind = deref->base_kind;
  base.name = (char *)deref->base_name;

  long long scale = mode == 1 ? new_stride : cls->width;
  long long add_const = cls->new_offset;
  char *current = NULL;

  if (deref->has_index) {
    char *scaled = ir_layout_temp_name();
    if (!scaled) {
      return NULL;
    }
    IROperand scale_op = ir_operand_int(scale);
    if (!ir_layout_emit_binary(vec, location, "*", scaled, &deref->index,
                               &scale_op)) {
      free(scaled);
      return NULL;
    }
    char *summed = ir_layout_temp_name();
    if (!summed) {
      free(scaled);
      return NULL;
    }
    IROperand scaled_op = ir_operand_temp(scaled);
    free(scaled);
    if (!scaled_op.name) {
      free(summed);
      return NULL;
    }
    int ok = ir_layout_emit_binary(vec, location, "+", summed, &base,
                                   &scaled_op);
    ir_operand_destroy(&scaled_op);
    if (!ok) {
      free(summed);
      return NULL;
    }
    current = summed;
  }

  if (add_const == 0 && current) {
    return current;
  }
  if (add_const == 0 && !current) {
    /* Address is exactly the base: reuse it via a plain copy temp so the
     * LOAD/STORE operand rewrite stays uniform. */
    char *copy = ir_layout_temp_name();
    if (!copy) {
      return NULL;
    }
    IROperand zero = ir_operand_int(0);
    if (!ir_layout_emit_binary(vec, location, "+", copy, &base, &zero)) {
      free(copy);
      return NULL;
    }
    return copy;
  }

  char *final_name = ir_layout_temp_name();
  if (!final_name) {
    free(current);
    return NULL;
  }
  IROperand addend = ir_operand_int(add_const);
  int ok;
  if (current) {
    IROperand cur_op = ir_operand_temp(current);
    free(current);
    if (!cur_op.name) {
      free(final_name);
      return NULL;
    }
    ok = ir_layout_emit_binary(vec, location, "+", final_name, &cur_op,
                               &addend);
    ir_operand_destroy(&cur_op);
  } else {
    ok = ir_layout_emit_binary(vec, location, "+", final_name, &base, &addend);
  }
  if (!ok) {
    free(final_name);
    return NULL;
  }
  return final_name;
}

/* Rewrite one function: rebuild the stream, inserting a fresh address chain
 * before each recorded deref and pointing the LOAD/STORE at it. The old
 * chains die and later dead-temp cleanup sweeps them. */
static int ir_layout_rewrite_function(IRLayoutCandidate *cand, IRFunction *fn,
                                      int mode, long long new_stride) {
  int any = 0;
  for (size_t d = 0; d < cand->deref_count; d++) {
    if (cand->derefs[d].fn == fn) {
      any = 1;
      break;
    }
  }
  if (!any) {
    return 1;
  }

  IRInstructionVector vec = {0};
  int ok = 1;
  for (size_t i = 0; i < fn->instruction_count && ok; i++) {
    IRInstruction *insn = &fn->instructions[i];
    const IRLayoutDeref *deref = NULL;
    for (size_t d = 0; d < cand->deref_count; d++) {
      if (cand->derefs[d].fn == fn && cand->derefs[d].insn_index == i) {
        deref = &cand->derefs[d];
        break;
      }
    }
    if (!deref) {
      IRInstruction cloned = {0};
      if (!ir_clone_instruction_plain(insn, &cloned) ||
          !ir_instruction_vector_append_move(&vec, &cloned)) {
        ir_instruction_destroy_storage(&cloned);
        ok = 0;
      }
      continue;
    }

    const IRLayoutClass *cls = &cand->classes[deref->class_index];
    char *addr = ir_layout_emit_address(&vec, deref, cls, mode, new_stride,
                                        insn->location);
    if (!addr) {
      ok = 0;
      continue;
    }
    IRInstruction cloned = {0};
    if (!ir_clone_instruction_plain(insn, &cloned)) {
      ir_instruction_destroy_storage(&cloned);
      free(addr);
      ok = 0;
      continue;
    }
    IROperand *slot = deref->is_store ? &cloned.dest : &cloned.lhs;
    ir_operand_destroy(slot);
    *slot = ir_operand_temp(addr);
    free(addr);
    if (!slot->name || !ir_instruction_vector_append_move(&vec, &cloned)) {
      ir_instruction_destroy_storage(&cloned);
      ok = 0;
    }
  }

  if (!ok) {
    ir_instruction_vector_destroy(&vec);
    return 0;
  }
  return ir_function_replace_instructions(fn, &vec);
}

/* Analyze one malloc site. Returns 1 and fills the candidate when the pool
 * qualifies (analysis only; no rewrite). */
static int ir_layout_analyze_candidate(IRLayoutCandidate *cand,
                                       IRProgram *program, IRFunction *fn,
                                       size_t call_index) {
  const IRInstruction *call = &fn->instructions[call_index];
  memset(cand, 0, sizeof(*cand));
  cand->program = program;
  cand->alloc_fn = fn;
  cand->alloc_index = call_index;

  IRSymbolValueMap symbol_map;
  if (!ir_temp_value_map_init(&symbol_map)) {
    return 0;
  }
  ir_build_symbol_int_map_before(fn, call_index, &symbol_map);
  long long size = 0;
  int resolved = ir_layout_resolve_const(fn, call_index, &symbol_map,
                                         &call->arguments[0], 0, &size);
  ir_temp_value_map_destroy(&symbol_map);
  if (!resolved || size <= 0) {
    ir_layout_debug("malloc in %s: size not compile-time (resolved=%lld)",
                    fn->name, resolved);
    return 0;
  }
  cand->alloc_size = size;
  ir_layout_debug("malloc in %s: candidate, size %lld", fn->name, size);

  if (!call->dest.name || (call->dest.kind != IR_OPERAND_TEMP &&
                           call->dest.kind != IR_OPERAND_SYMBOL)) {
    return 0;
  }
  if (!ir_layout_alias_add(cand, fn, call->dest.name, call->dest.kind)) {
    return 0;
  }
  if (cand->func_count >= IR_LAYOUT_MAX_FUNCS) {
    return 0;
  }
  cand->funcs[cand->func_count++] = fn;

  /* Alias closure to a true fixpoint: growing one function's alias set can
   * turn a call argument elsewhere into an alias, which pulls in a new callee,
   * whose param alias can unlock more derived records anywhere. Iterate
   * grow-all + peek-all until nothing changes, THEN validate everything.
   * Validation refuses any use the closure didn't already know about. */
  for (int round = 0; round < IR_LAYOUT_MAX_FUNCS + 2; round++) {
    size_t before_aliases = cand->alias_count;
    size_t before_funcs = cand->func_count;
    for (size_t k = 0; k < cand->func_count; k++) {
      if (!ir_layout_grow_function(cand, cand->funcs[k])) {
        ir_layout_debug("grow failed in %s (%lld)", cand->funcs[k]->name,
                        (long long)k);
        return 0;
      }
    }
    for (size_t k = 0; k < cand->func_count; k++) {
      IRFunction *cur = cand->funcs[k];
      for (size_t i = 0; i < cur->instruction_count; i++) {
        const IRInstruction *insn = &cur->instructions[i];
        if (insn->op != IR_OP_CALL || !insn->text) {
          continue;
        }
        if (cur == cand->alloc_fn && i == cand->alloc_index) {
          continue;
        }
        for (size_t a = 0; a < insn->argument_count; a++) {
          if (!ir_layout_operand_is_alias(cand, cur, &insn->arguments[a])) {
            continue;
          }
          if (strcmp(insn->text, "free") == 0) {
            continue;
          }
          IRFunction *callee =
              ir_program_find_function(program, insn->text);
          if (!callee || a >= callee->parameter_count ||
              !callee->parameter_names[a]) {
            ir_layout_debug("pool passed to extern/unknown %s (%lld)",
                            insn->text, (long long)a);
            return 0;
          }
          if (!ir_layout_alias_add(cand, callee, callee->parameter_names[a],
                                   IR_OPERAND_SYMBOL)) {
            return 0;
          }
          if (!ir_layout_func_tracked(cand, callee)) {
            if (cand->func_count >= IR_LAYOUT_MAX_FUNCS) {
              return 0;
            }
            cand->funcs[cand->func_count++] = callee;
          }
        }
      }
    }
    if (cand->alias_count == before_aliases &&
        cand->func_count == before_funcs) {
      break;
    }
  }

  for (size_t k = 0; k < cand->func_count; k++) {
    if (!ir_layout_validate_function(cand, cand->funcs[k])) {
      ir_layout_debug("validate failed in %s (fn %lld)",
                      cand->funcs[k]->name, (long long)k);
      return 0;
    }
  }

  if (cand->stride <= 0 || cand->class_count < 2 ||
      cand->alloc_size % cand->stride != 0) {
    ir_layout_debug("shape reject: stride %s=%lld", "",
                    cand->stride);
    ir_layout_debug("shape reject: classes %s=%lld", "",
                    (long long)cand->class_count);
    return 0;
  }
  qsort(cand->classes, cand->class_count, sizeof(cand->classes[0]),
        ir_layout_class_cmp_offset);
  for (size_t i = 0; i < cand->class_count; i++) {
    ir_layout_debug("class %s offset=%lld", "", cand->classes[i].offset);
    if (cand->classes[i].offset + cand->classes[i].width > cand->stride) {
      ir_layout_debug("class beyond stride %s (%lld)", "", cand->stride);
      return 0;
    }
    if (i + 1 < cand->class_count &&
        cand->classes[i].offset + cand->classes[i].width >
            cand->classes[i + 1].offset) {
      ir_layout_debug("overlapping fields %s (%lld)", "",
                      cand->classes[i].offset);
      return 0; /* overlapping fields */
    }
  }
  /* Re-key each deref to its (now sorted) class. */
  for (size_t d = 0; d < cand->deref_count; d++) {
    int found = 0;
    for (size_t c = 0; c < cand->class_count; c++) {
      if (cand->classes[c].offset == cand->derefs[d].offset) {
        cand->derefs[d].class_index = c;
        found = 1;
        break;
      }
    }
    if (!found) {
      return 0;
    }
  }

  if (!ir_layout_check_call_completeness(cand)) {
    ir_layout_debug("call completeness failed %s (%lld edges)", "",
                    (long long)cand->edge_count);
    return 0;
  }
  ir_layout_debug("qualified: stride %s=%lld", "", cand->stride);
  return 1;
}

int ir_layout_factor_pass(IRProgram *program, int *changed) {
  if (!program) {
    return 0;
  }

  /* Claimed malloc sites (keyed by function + result name, which rewrites
   * never rename) so a transformed pool is not re-analyzed this run. The
   * name is copied out because the rewrite replaces the instruction array. */
  const IRFunction *claimed_fn[8];
  char claimed_dest[8][64];
  size_t claimed_count = 0;

  for (size_t f = 0; f < program->function_count; f++) {
    IRFunction *fn = program->functions[f];
    for (size_t i = 0; i < fn->instruction_count; i++) {
      const IRInstruction *insn = &fn->instructions[i];
      if (insn->op != IR_OP_CALL || !insn->text ||
          strcmp(insn->text, "malloc") != 0 || insn->argument_count != 1 ||
          !insn->dest.name) {
        continue;
      }
      int already = 0;
      for (size_t c = 0; c < claimed_count; c++) {
        if (claimed_fn[c] == fn &&
            strcmp(claimed_dest[c], insn->dest.name) == 0) {
          already = 1;
          break;
        }
      }
      if (already) {
        continue;
      }

      IRLayoutCandidate *cand =
          (IRLayoutCandidate *)calloc(1, sizeof(IRLayoutCandidate));
      if (!cand) {
        return 0;
      }
      if (!ir_layout_analyze_candidate(cand, program, fn, i)) {
        free(cand);
        continue;
      }

      /* Capture site facts now: the rewrite replaces instruction arrays and
       * `insn` dangles afterwards. */
      char site_dest[64];
      snprintf(site_dest, sizeof(site_dest), "%s", insn->dest.name);
      SourceLocation site_location = insn->location;

      long long n = cand->alloc_size / cand->stride;

      /* Mode selection: SOA when the program's loads touch a strict subset
       * of the fields (traversals can skip the cold arrays); otherwise
       * COMPACT when packing removes padding; otherwise leave it alone. */
      size_t load_classes = 0;
      long long packed_width = 0;
      for (size_t c = 0; c < cand->class_count; c++) {
        if (cand->classes[c].seen_in_load) {
          load_classes++;
        }
        packed_width += cand->classes[c].width;
      }
      int mode = 0;
      long long new_stride = 0;
      if (load_classes > 0 && load_classes < cand->class_count) {
        mode = 2;
        ir_layout_assign_offsets(cand, 2, n);
      } else {
        long long packed = ir_layout_assign_offsets(cand, 1, n);
        if (packed < cand->stride) {
          mode = 1;
          new_stride = packed;
        }
      }
      ir_layout_debug("mode %s=%lld (1=compact 2=soa)", "", (long long)mode);
      if (mode == 0) {
        free(cand);
        continue;
      }

      int rewrite_ok = 1;
      for (size_t k = 0; k < cand->func_count && rewrite_ok; k++) {
        rewrite_ok =
            ir_layout_rewrite_function(cand, cand->funcs[k], mode, new_stride);
      }
      if (!rewrite_ok) {
        free(cand);
        return 0;
      }

      if (ir_explain_enabled()) {
        char headline[128];
        if (mode == 1) {
          snprintf(headline, sizeof(headline),
                   "pool layout compacted: %lld -> %lld byte stride "
                   "(%zu fields, %lld elements)",
                   cand->stride, new_stride, cand->class_count, n);
        } else {
          snprintf(headline, sizeof(headline),
                   "pool layout factored into %zu field arrays (SoA, "
                   "%lld elements)",
                   cand->class_count, n);
        }
        ir_explain_remark(fn->name, "allocation", site_location, 1, headline,
                          NULL, NULL, NULL);
      }

      if (claimed_count < IR_ARRAY_COUNT(claimed_fn)) {
        claimed_fn[claimed_count] = fn;
        snprintf(claimed_dest[claimed_count], sizeof(claimed_dest[0]), "%s",
                 site_dest);
        claimed_count++;
      }
      if (changed) {
        *changed = 1;
      }
      free(cand);
      /* The rewrite replaced this function's instruction array (and possibly
       * other functions'); restart the scan of this function so the loop
       * index never walks a stale array. Claims stop re-transforms. */
      i = (size_t)-1;
    }
  }
  return 1;
}
