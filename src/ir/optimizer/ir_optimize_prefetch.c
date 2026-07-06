#include "ir_optimize_internal.h"

/* ---- Software prefetching for indirect memory accesses ----------------------
 *
 * A counted loop whose body loads A[f(B[i])] pays the full memory latency on
 * every A-access: the address depends on a value loaded this iteration, so
 * neither the hardware prefetcher (random address stream) nor out-of-order
 * execution (the chain is serial) can hide it. But B[i + D] is already
 * computable D iterations early. Following Ainsworth & Jones (CGO'17), this
 * pass duplicates the address-computation slice at distance D and issues an
 * advisory prefetcht0 for the future element:
 *
 *   t  = i + D
 *   if (t < n) {                 // same bound as the loop: B[t] stays in-bounds
 *     addr = A + f(B[t])         // the cloned slice; B-load is a real load
 *     prefetch addr              // never faults; A[garbage] costs nothing
 *   }
 *
 * The clone's B-load is guarded by the loop's own bound, so it reads only
 * elements the loop itself will read. The A-prefetch needs no guard.
 *
 * Strictly shape-gated: counted `while (i < n)` with a unit increment, iv
 * provably 0 at entry, straight-line body (no other control flow), and an
 * address slice containing exactly one interior load. Runs LAST in the
 * post-fixpoint stage so every vectorizer has had its chance first (loops
 * they claim are gone by now; loops with indirect loads are ones they can
 * never claim). METTLE_PREFETCH_DIST overrides the distance (default 16). */

#define IR_PREFETCH_MAX_SLICE 12
#define IR_PREFETCH_DEFAULT_DIST 64

static size_t g_prefetch_id;

static long long ir_prefetch_distance_override(void) {
  static long long cached = -1;
  if (cached < 0) {
    const char *env = getenv("METTLE_PREFETCH_DIST");
    cached = 0;
    if (!env || !*env) {
      return -1;
    }
    long long v = atoll(env);
    if (v >= 1 && v <= 4096) {
      cached = v;
    }
  }
  return cached > 0 ? cached : -1;
}

static long long ir_prefetch_distance_for_loop(const IRFunction *function,
                                               SourceLocation location) {
  long long override = ir_prefetch_distance_override();
  if (override > 0) {
    return override;
  }
  return ir_opt_prefetch_distance_for_site(function, location,
                                          IR_PREFETCH_DEFAULT_DIST);
}

/* Is `name` written anywhere in [start, end)? Loop-invariant bases must not
 * change between the real access and the look-ahead clone. */
static int ir_prefetch_symbol_written_in(const IRFunction *function,
                                         size_t start, size_t end,
                                         const char *name) {
  for (size_t i = start; i < end; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ir_instruction_writes_destination(ins) &&
        ir_operand_is_symbol_named(&ins->dest, name)) {
      return 1;
    }
  }
  return 0;
}

typedef struct {
  size_t indices[IR_PREFETCH_MAX_SLICE];
  size_t count;
  size_t interior_loads;
} IRPrefetchSlice;

static int ir_prefetch_slice_contains(const IRPrefetchSlice *slice,
                                      size_t index) {
  for (size_t i = 0; i < slice->count; i++) {
    if (slice->indices[i] == index) {
      return 1;
    }
  }
  return 0;
}

/* Collect the producer slice of `temp` (searching backwards from `at`, staying
 * inside the loop body [body_start, at]). An operand terminates recursion when
 * it is: an INT literal, the loop iv, or a symbol not written in the body
 * (loop-invariant base). Every slice instruction must be a BINARY/CAST/LOAD
 * over such operands. Returns 0 if the slice is unbounded or ill-shaped. */
static int ir_prefetch_collect_slice(const IRFunction *function,
                                     size_t body_start, size_t body_end,
                                     size_t at, const char *temp,
                                     const char *iv_symbol,
                                     IRPrefetchSlice *slice) {
  const IRInstruction *producer =
      ir_find_temp_producer_before(function, at, temp);
  if (!producer) {
    return 0;
  }
  size_t prod_index = (size_t)(producer - function->instructions);
  if (prod_index < body_start || prod_index >= body_end) {
    return 0; /* address computed outside the body: not per-iteration */
  }
  if (ir_prefetch_slice_contains(slice, prod_index)) {
    return 1;
  }
  if (slice->count >= IR_PREFETCH_MAX_SLICE) {
    return 0;
  }

  if (producer->op != IR_OP_BINARY && producer->op != IR_OP_CAST &&
      producer->op != IR_OP_LOAD) {
    return 0;
  }
  if (producer->is_float) {
    return 0;
  }
  if (producer->op == IR_OP_LOAD) {
    slice->interior_loads++;
  }

  const IROperand *sources[2] = {&producer->lhs, NULL};
  size_t source_count = 1;
  if (producer->op == IR_OP_BINARY) {
    sources[1] = &producer->rhs;
    source_count = 2;
  }
  for (size_t s = 0; s < source_count; s++) {
    const IROperand *op = sources[s];
    if (op->kind == IR_OPERAND_INT) {
      continue;
    }
    if (op->kind == IR_OPERAND_SYMBOL && op->name) {
      if (strcmp(op->name, iv_symbol) == 0) {
        continue;
      }
      if (!ir_prefetch_symbol_written_in(function, body_start, body_end,
                                         op->name)) {
        continue; /* loop-invariant base (array pointer, bound, scale) */
      }
      return 0;
    }
    if (op->kind == IR_OPERAND_TEMP && op->name) {
      if (!ir_prefetch_collect_slice(function, body_start, body_end,
                                     prod_index, op->name, iv_symbol, slice)) {
        return 0;
      }
      continue;
    }
    return 0;
  }

  slice->indices[slice->count++] = prod_index;
  return 1;
}

/* The slice must actually depend on the iv (otherwise the address is loop-
 * invariant and prefetching is pointless) -- check any slice instruction
 * reads it. */
static int ir_prefetch_slice_uses_iv(const IRFunction *function,
                                     const IRPrefetchSlice *slice,
                                     const char *iv_symbol) {
  for (size_t i = 0; i < slice->count; i++) {
    const IRInstruction *ins = &function->instructions[slice->indices[i]];
    if (ir_operand_is_symbol_named(&ins->lhs, iv_symbol) ||
        ir_operand_is_symbol_named(&ins->rhs, iv_symbol)) {
      return 1;
    }
  }
  return 0;
}

static char *ir_prefetch_temp_name(void) {
  char buf[32];
  snprintf(buf, sizeof(buf), ".pf%zu", g_prefetch_id++);
  return mettle_strdup(buf);
}

/* Append a clone of `src` into `vec` with dest renamed to a fresh temp and
 * every TEMP operand that names an earlier slice dest rewritten via `names`,
 * plus iv -> lookahead substitution. Returns the fresh dest name (borrowed
 * from the appended instruction) or NULL. */
static const char *ir_prefetch_emit_clone(IRInstructionVector *vec,
                                          const IRInstruction *src,
                                          IRNameMap *names,
                                          const char *iv_symbol,
                                          const char *lookahead_temp) {
  IRInstruction cloned = {0};
  if (!ir_clone_instruction_plain(src, &cloned)) {
    ir_instruction_destroy_storage(&cloned);
    return NULL;
  }
  IROperand *ops[2] = {&cloned.lhs, &cloned.rhs};
  for (int k = 0; k < 2; k++) {
    IROperand *op = ops[k];
    if (op->kind == IR_OPERAND_TEMP && op->name) {
      const char *mapped = ir_name_map_lookup(names, op->name);
      if (mapped) {
        free(op->name);
        op->name = mettle_strdup(mapped);
        if (!op->name) {
          ir_instruction_destroy_storage(&cloned);
          return NULL;
        }
      }
    } else if (op->kind == IR_OPERAND_SYMBOL && op->name &&
               strcmp(op->name, iv_symbol) == 0) {
      /* iv -> the look-ahead index temp */
      free(op->name);
      op->kind = IR_OPERAND_TEMP;
      op->name = mettle_strdup(lookahead_temp);
      if (!op->name) {
        ir_instruction_destroy_storage(&cloned);
        return NULL;
      }
    }
  }
  char *fresh = ir_prefetch_temp_name();
  if (!fresh || !ir_name_map_add(names, cloned.dest.name, fresh)) {
    free(fresh);
    ir_instruction_destroy_storage(&cloned);
    return NULL;
  }
  ir_operand_destroy(&cloned.dest);
  cloned.dest = ir_operand_temp(fresh);
  free(fresh);
  if (!cloned.dest.name || !ir_instruction_vector_append_move(vec, &cloned)) {
    ir_instruction_destroy_storage(&cloned);
    return NULL;
  }
  return vec->items[vec->count - 1].dest.name;
}

typedef struct {
  size_t insert_after; /* branch index: sequence goes at the body top */
  IRInstructionVector seq;
} IRPrefetchPlan;

/* Try to plan a prefetch for the counted loop at header_index. On success the
 * plan's instruction sequence is filled and 1 is returned. */
static int ir_prefetch_plan_loop(IRFunction *function, size_t header_index,
                                 IRPrefetchPlan *plan) {
  IRWhileLoopBounds bounds;
  if (!ir_find_while_loop_bounds(function, header_index, &bounds)) {
    return 0;
  }
  const IRInstruction *compare = &function->instructions[bounds.compare_index];
  if (compare->op != IR_OP_BINARY || compare->is_float || !compare->text ||
      strcmp(compare->text, "<") != 0 ||
      compare->lhs.kind != IR_OPERAND_SYMBOL || !compare->lhs.name) {
    return 0;
  }
  if (compare->rhs.kind != IR_OPERAND_SYMBOL &&
      compare->rhs.kind != IR_OPERAND_INT) {
    return 0;
  }
  const char *iv_symbol = compare->lhs.name;
  size_t body_start = bounds.branch_index + 1;
  size_t body_end = bounds.jump_index;

  /* Straight-line body only: an early exit or interior branch could make the
   * cloned B-load read an element the loop never touches. */
  for (size_t i = body_start; i < body_end; i++) {
    IROpcode op = function->instructions[i].op;
    if (op == IR_OP_LABEL || op == IR_OP_JUMP || op == IR_OP_BRANCH_ZERO ||
        op == IR_OP_BRANCH_EQ || op == IR_OP_CALL ||
        op == IR_OP_CALL_INDIRECT || op == IR_OP_PREFETCH) {
      return 0;
    }
  }
  /* Unit increment + iv from 0 (keeps i+D bounded by n+D, no wrap). */
  int has_increment = 0;
  for (size_t i = body_start; i < body_end; i++) {
    if (ir_try_parse_direct_unit_increment(&function->instructions[i],
                                           iv_symbol)) {
      has_increment = 1;
      break;
    }
  }
  if (!has_increment ||
      !ir_iv_zero_at_header(function, header_index, iv_symbol)) {
    return 0;
  }
  /* The bound must be loop-invariant. */
  if (compare->rhs.kind == IR_OPERAND_SYMBOL &&
      ir_prefetch_symbol_written_in(function, body_start, body_end,
                                    compare->rhs.name)) {
    return 0;
  }

  /* Find the indirect load: a LOAD whose address slice contains exactly one
   * interior load and depends on the iv. Take the first such. */
  size_t target_load = (size_t)-1;
  IRPrefetchSlice slice;
  for (size_t i = body_start; i < body_end; i++) {
    const IRInstruction *ins = &function->instructions[i];
    if (ins->op != IR_OP_LOAD || ins->lhs.kind != IR_OPERAND_TEMP ||
        !ins->lhs.name) {
      continue;
    }
    memset(&slice, 0, sizeof(slice));
    if (!ir_prefetch_collect_slice(function, body_start, body_end, i,
                                   ins->lhs.name, iv_symbol, &slice)) {
      continue;
    }
    if (slice.interior_loads == 1 &&
        ir_prefetch_slice_uses_iv(function, &slice, iv_symbol)) {
      target_load = i;
      break;
    }
  }
  if (target_load == (size_t)-1) {
    return 0;
  }

  /* Slice indices were pushed post-order (leaves first), so emitting in
   * recorded order respects dependencies. */
  IRNameMap names = {0};
  IRInstructionVector *seq = &plan->seq;
  memset(seq, 0, sizeof(*seq));
  SourceLocation loc = function->instructions[target_load].location;
  if (ir_prefetch_distance_override() < 0 &&
      !ir_opt_should_prefetch_site(function, loc)) {
    return 0;
  }
  long long D = ir_prefetch_distance_for_loop(function, loc);

  char *ahead = ir_prefetch_temp_name();
  char *cond = ir_prefetch_temp_name();
  char skip_label[48];
  snprintf(skip_label, sizeof(skip_label), "ir_pf_skip_%zu", g_prefetch_id++);
  int ok = ahead && cond;

  /* ahead = iv + D */
  if (ok) {
    IRInstruction add = {0};
    add.op = IR_OP_BINARY;
    add.location = loc;
    add.text = mettle_strdup("+");
    add.dest = ir_operand_temp(ahead);
    add.lhs = ir_operand_symbol(iv_symbol);
    add.rhs = ir_operand_int(D);
    ok = add.text && add.dest.name && add.lhs.name &&
         ir_instruction_vector_append_move(seq, &add);
    if (!ok) {
      ir_instruction_destroy_storage(&add);
    }
  }
  /* cond = ahead < bound */
  if (ok) {
    IRInstruction cmp = {0};
    cmp.op = IR_OP_BINARY;
    cmp.location = loc;
    cmp.text = mettle_strdup("<");
    cmp.dest = ir_operand_temp(cond);
    cmp.lhs = ir_operand_temp(ahead);
    if (compare->rhs.kind == IR_OPERAND_INT) {
      cmp.rhs = ir_operand_int(compare->rhs.int_value);
    } else {
      cmp.rhs = ir_operand_symbol(compare->rhs.name);
    }
    ok = cmp.text && cmp.dest.name && cmp.lhs.name &&
         (cmp.rhs.kind == IR_OPERAND_INT || cmp.rhs.name) &&
         ir_instruction_vector_append_move(seq, &cmp);
    if (!ok) {
      ir_instruction_destroy_storage(&cmp);
    }
  }
  /* branch_zero cond -> skip */
  if (ok) {
    IRInstruction br = {0};
    br.op = IR_OP_BRANCH_ZERO;
    br.location = loc;
    br.lhs = ir_operand_temp(cond);
    br.text = mettle_strdup(skip_label);
    ok = br.lhs.name && br.text && ir_instruction_vector_append_move(seq, &br);
    if (!ok) {
      ir_instruction_destroy_storage(&br);
    }
  }
  /* The cloned slice with iv -> ahead. */
  const char *final_addr = NULL;
  for (size_t s = 0; ok && s < slice.count; s++) {
    final_addr = ir_prefetch_emit_clone(
        seq, &function->instructions[slice.indices[s]], &names, iv_symbol,
        ahead);
    ok = final_addr != NULL;
  }
  /* prefetch <cloned address of the target load>. */
  if (ok) {
    const char *addr_name = ir_name_map_lookup(
        &names, function->instructions[target_load].lhs.name);
    ok = addr_name != NULL;
    if (ok) {
      IRInstruction pf = {0};
      pf.op = IR_OP_PREFETCH;
      pf.location = loc;
      pf.lhs = ir_operand_temp(addr_name);
      ok = pf.lhs.name && ir_instruction_vector_append_move(seq, &pf);
      if (!ok) {
        ir_instruction_destroy_storage(&pf);
      }
    }
  }
  /* skip: */
  if (ok) {
    IRInstruction lbl = {0};
    lbl.op = IR_OP_LABEL;
    lbl.location = loc;
    lbl.text = mettle_strdup(skip_label);
    ok = lbl.text && ir_instruction_vector_append_move(seq, &lbl);
    if (!ok) {
      ir_instruction_destroy_storage(&lbl);
    }
  }

  free(ahead);
  free(cond);
  ir_name_map_destroy(&names);
  if (!ok) {
    ir_instruction_vector_destroy(seq);
    return 0;
  }
  plan->insert_after = bounds.branch_index;
  return 1;
}

int ir_prefetch_indirect_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    if (function->instructions[i].op != IR_OP_LABEL ||
        !ir_label_is_while_header(function->instructions[i].text)) {
      continue;
    }
    IRPrefetchPlan plan;
    memset(&plan, 0, sizeof(plan));
    if (!ir_prefetch_plan_loop(function, i, &plan)) {
      continue;
    }

    /* Rebuild the stream with the sequence spliced in after the loop branch. */
    IRInstructionVector vec = {0};
    int ok = 1;
    for (size_t k = 0; k < function->instruction_count && ok; k++) {
      IRInstruction cloned = {0};
      if (!ir_clone_instruction_plain(&function->instructions[k], &cloned) ||
          !ir_instruction_vector_append_move(&vec, &cloned)) {
        ir_instruction_destroy_storage(&cloned);
        ok = 0;
        break;
      }
      if (k == plan.insert_after) {
        for (size_t s = 0; s < plan.seq.count && ok; s++) {
          IRInstruction c2 = {0};
          if (!ir_clone_instruction_plain(&plan.seq.items[s], &c2) ||
              !ir_instruction_vector_append_move(&vec, &c2)) {
            ir_instruction_destroy_storage(&c2);
            ok = 0;
          }
        }
      }
    }
    ir_instruction_vector_destroy(&plan.seq);
    if (!ok) {
      ir_instruction_vector_destroy(&vec);
      return 0;
    }
    if (!ir_function_replace_instructions(function, &vec)) {
      ir_instruction_vector_destroy(&vec);
      return 0;
    }
    if (ir_explain_enabled()) {
      ir_explain_remark(function->name, "loop",
                        function->instructions[i].location, 1,
                        "software prefetch inserted for indirect access "
                        "(look-ahead distance covers the miss latency)",
                        NULL, NULL, NULL);
    }
    if (changed) {
      *changed = 1;
    }
    /* Skip past this loop: i now points at the same header (clone kept it). */
  }
  return 1;
}
