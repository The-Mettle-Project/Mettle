#include "ir_optimize_internal.h"
#include "../../common.h"

#define VR_MIN LLONG_MIN
#define VR_MAX LLONG_MAX
#define VR_MAX_DEPTH 4
#define VR_MAX_SCAN 192

static void vr_full(IRIntRange *r) {
  r->lo = VR_MIN;
  r->hi = VR_MAX;
}

static int vr_is_full(const IRIntRange *r) {
  return r->lo == VR_MIN && r->hi == VR_MAX;
}

static void vr_intersect(IRIntRange *r, const IRIntRange *other) {
  long long lo = other->lo > r->lo ? other->lo : r->lo;
  long long hi = other->hi < r->hi ? other->hi : r->hi;
  if (lo <= hi) {
    r->lo = lo;
    r->hi = hi;
  }
}

static void vr_narrow_to(IRIntRange *r, const IRIntRange *limit) {
  if (vr_is_full(limit)) {
    return;
  }
  if (r->lo >= limit->lo && r->hi <= limit->hi) {
    return;
  }
  *r = *limit;
}

static void vr_of_int_type(int bits, int is_unsigned, IRIntRange *r) {
  if (bits <= 0 || bits > 64 || (bits == 64 && is_unsigned)) {
    vr_full(r);
    return;
  }
  if (bits == 64) {
    vr_full(r);
    return;
  }
  if (is_unsigned) {
    r->lo = 0;
    r->hi = (long long)((1ull << bits) - 1ull);
  } else {
    r->hi = (long long)((1ull << (bits - 1)) - 1ull);
    r->lo = -r->hi - 1;
  }
}

int ir_int_type_name_info(const char *name, int *bits_out,
                          int *is_unsigned_out) {
  int bits = 0;
  int is_unsigned = 0;

  if (!name) {
    return 0;
  }
  if (strcmp(name, "int8") == 0) {
    bits = 8;
  } else if (strcmp(name, "uint8") == 0) {
    bits = 8;
    is_unsigned = 1;
  } else if (strcmp(name, "int16") == 0) {
    bits = 16;
  } else if (strcmp(name, "uint16") == 0) {
    bits = 16;
    is_unsigned = 1;
  } else if (strcmp(name, "int32") == 0) {
    bits = 32;
  } else if (strcmp(name, "uint32") == 0) {
    bits = 32;
    is_unsigned = 1;
  } else if (strcmp(name, "int64") == 0) {
    bits = 64;
  } else if (strcmp(name, "uint64") == 0) {
    bits = 64;
    is_unsigned = 1;
  } else {
    return 0;
  }

  if (bits_out) {
    *bits_out = bits;
  }
  if (is_unsigned_out) {
    *is_unsigned_out = is_unsigned;
  }
  return 1;
}

#define VR_TYPE_ENCODE(bits, uns) (((long long)(bits) << 2) | ((uns) ? 2 : 0) | 1)
#define VR_TYPE_BITS(enc) ((int)((enc) >> 2))
#define VR_TYPE_UNSIGNED(enc) (((enc) & 2) != 0)

void ir_value_range_ctx_init(IRValueRangeCtx *ctx, const IRFunction *function) {
  if (!ctx) {
    return;
  }
  ctx->function = function;
  ctx->built = 0;
  ctx->ok = 0;
}

static void vr_symbol_sites_drop(IRValueRangeCtx *ctx) {
  if (ctx->symbol_sites_built) {
    ir_name_index_destroy(&ctx->symbol_groups);
  }
  for (size_t i = 0; i < ctx->symbol_name_count; i++) {
    free(ctx->symbol_names[i]);
  }
  free(ctx->symbol_names);
  free(ctx->symbol_sites);
  free(ctx->symbol_group_start);
  ctx->symbol_names = NULL;
  ctx->symbol_name_count = 0;
  ctx->symbol_sites = NULL;
  ctx->symbol_group_start = NULL;
  ctx->symbol_sites_built = 0;
}

void ir_value_range_ctx_destroy(IRValueRangeCtx *ctx) {
  if (!ctx || !ctx->built) {
    return;
  }
  ir_temp_value_map_destroy(&ctx->decl_types);
  ir_temp_value_map_destroy(&ctx->addr_taken);
  ir_temp_value_map_destroy(&ctx->monotone);
  ir_temp_value_map_destroy(&ctx->counter_lo);
  ir_temp_value_map_destroy(&ctx->counter_hi);
  ir_temp_value_map_destroy(&ctx->label_guard);
  ir_temp_value_map_destroy(&ctx->unique_def);
  ir_temp_value_map_destroy(&ctx->label_refs);
  vr_symbol_sites_drop(ctx);
  ctx->built = 0;
  ctx->ok = 0;
}

static int vr_ctx_populate(IRValueRangeCtx *ctx) {
  const IRFunction *fn = ctx->function;

  if (!ir_addr_taken_set_build(fn, &ctx->addr_taken)) {
    return 0;
  }
  for (size_t i = 0; i < fn->parameter_count; i++) {
    int bits = 0, uns = 0;
    if (!fn->parameter_names || !fn->parameter_names[i] ||
        !fn->parameter_types || !fn->parameter_types[i] ||
        !ir_int_type_name_info(fn->parameter_types[i], &bits, &uns)) {
      continue;
    }
    IROperand value = ir_operand_int(VR_TYPE_ENCODE(bits, uns));
    if (!ir_temp_value_map_set(&ctx->decl_types, fn->parameter_names[i],
                               &value)) {
      return 0;
    }
  }
  for (size_t i = 0; i < fn->instruction_count; i++) {
    const IRInstruction *in = &fn->instructions[i];
    int bits = 0, uns = 0;
    if (in->op != IR_OP_DECLARE_LOCAL || in->dest.kind != IR_OPERAND_SYMBOL ||
        !in->dest.name || !ir_int_type_name_info(in->text, &bits, &uns)) {
      continue;
    }
    IROperand value = ir_operand_int(VR_TYPE_ENCODE(bits, uns));
    if (!ir_temp_value_map_set(&ctx->decl_types, in->dest.name, &value)) {
      return 0;
    }
  }
  /* A temp written exactly once carries that instruction's value wherever it
     is read, so a reader in another block can still see it. The loop bound a
     scanner compares against is loaded in the entry block, and without this
     nothing downstream knows it is an int32. */
  for (size_t i = 0; i < fn->instruction_count; i++) {
    const IRInstruction *in = &fn->instructions[i];
    if (!ir_instruction_writes_destination(in) ||
        in->dest.kind != IR_OPERAND_TEMP || !in->dest.name) {
      continue;
    }
    const IROperand *seen =
        ir_temp_value_map_lookup(&ctx->unique_def, in->dest.name);
    IROperand value = ir_operand_int(seen ? -1 : (long long)i + 1);
    if (!ir_temp_value_map_set(&ctx->unique_def, in->dest.name, &value)) {
      return 0;
    }
  }
  for (size_t i = 0; i < fn->instruction_count; i++) {
    const IRInstruction *in = &fn->instructions[i];
    if (!in->text || (in->op != IR_OP_JUMP && in->op != IR_OP_BRANCH_ZERO &&
                      in->op != IR_OP_BRANCH_EQ)) {
      continue;
    }
    const IROperand *seen = ir_temp_value_map_lookup(&ctx->label_refs, in->text);
    IROperand value = ir_operand_int(
        seen ? -2 : (in->op == IR_OP_BRANCH_ZERO ? (long long)i + 1 : -1));
    if (!ir_temp_value_map_set(&ctx->label_refs, in->text, &value)) {
      return 0;
    }
  }
  return 1;
}

static int vr_ctx_build(IRValueRangeCtx *ctx) {
  if (ctx->built) {
    return ctx->ok;
  }
  ctx->built = 1;
  ctx->ok = 0;
  ctx->symbol_names = NULL;
  ctx->symbol_name_count = 0;
  ctx->symbol_sites = NULL;
  ctx->symbol_group_start = NULL;
  ctx->symbol_sites_built = 0;
  if (!ctx->function) {
    return 0;
  }
  if (!ir_temp_value_map_init(&ctx->decl_types) ||
      !ir_temp_value_map_init(&ctx->addr_taken) ||
      !ir_temp_value_map_init(&ctx->monotone) ||
      !ir_temp_value_map_init(&ctx->counter_lo) ||
      !ir_temp_value_map_init(&ctx->counter_hi) ||
      !ir_temp_value_map_init(&ctx->label_guard) ||
      !ir_temp_value_map_init(&ctx->unique_def) ||
      !ir_temp_value_map_init(&ctx->label_refs)) {
    ir_value_range_ctx_destroy(ctx);
    ctx->built = 1;
    return 0;
  }
  if (!vr_ctx_populate(ctx)) {
    ir_value_range_ctx_destroy(ctx);
    ctx->built = 1;
    return 0;
  }

  ctx->ok = 1;
  return 1;
}

static void vr_declared_range(IRValueRangeCtx *ctx, const char *symbol,
                              IRIntRange *out) {
  vr_full(out);
  if (!symbol) {
    return;
  }
  const IROperand *enc = ir_temp_value_map_lookup(&ctx->decl_types, symbol);
  if (!enc || enc->kind != IR_OPERAND_INT) {
    return;
  }
  vr_of_int_type(VR_TYPE_BITS(enc->int_value),
                 VR_TYPE_UNSIGNED(enc->int_value), out);
}

static int vr_symbol_address_taken(IRValueRangeCtx *ctx, const char *symbol) {
  return symbol && ir_temp_value_map_lookup(&ctx->addr_taken, symbol) != NULL;
}

static int vr_symbol_is_private(IRValueRangeCtx *ctx, const char *symbol) {
  return symbol && ir_temp_value_map_lookup(&ctx->decl_types, symbol) != NULL &&
         !vr_symbol_address_taken(ctx, symbol);
}

static int vr_clobbers_memory(const IRInstruction *in) {
  return in->op == IR_OP_CALL || in->op == IR_OP_CALL_INDIRECT ||
         in->op == IR_OP_STORE || in->op == IR_OP_INLINE_ASM;
}

static void vr_operand_range(IRValueRangeCtx *ctx, size_t at,
                             const IROperand *operand, int depth,
                             IRIntRange *out);

static int vr_find_block_writer(const IRFunction *fn, size_t at,
                                IROperandKind kind, const char *name,
                                int stop_at_clobber, size_t *out_index) {
  size_t scanned = 0;
  for (size_t i = at; i > 0 && scanned < VR_MAX_SCAN;) {
    i--;
    const IRInstruction *in = &fn->instructions[i];
    if (in->op == IR_OP_NOP) {
      continue;
    }
    scanned++;
    if (in->op == IR_OP_LABEL) {
      return 0;
    }
    if (in->dest.kind == kind && in->dest.name && name &&
        in->dest.name[0] == name[0] && strcmp(in->dest.name, name) == 0 &&
        ir_instruction_writes_destination(in)) {
      *out_index = i;
      return 1;
    }
    if (stop_at_clobber && vr_clobbers_memory(in)) {
      return 0;
    }
  }
  return 0;
}

static void vr_apply_relation(const char *op, int symbol_on_left,
                              const IRIntRange *bound, IRIntRange *r) {
  int gt = 0, ge = 0, lt = 0, le = 0, eq = 0;

  if (strcmp(op, ">") == 0) {
    gt = 1;
  } else if (strcmp(op, ">=") == 0) {
    ge = 1;
  } else if (strcmp(op, "<") == 0) {
    lt = 1;
  } else if (strcmp(op, "<=") == 0) {
    le = 1;
  } else if (strcmp(op, "==") == 0) {
    eq = 1;
  } else {
    return;
  }

  if (!symbol_on_left) {
    int t;
    t = gt; gt = lt; lt = t;
    t = ge; ge = le; le = t;
  }

  IRIntRange fact;
  vr_full(&fact);
  if (eq) {
    fact = *bound;
  } else if (gt) {
    if (bound->lo == VR_MAX) {
      return;
    }
    fact.lo = bound->lo + 1;
  } else if (ge) {
    fact.lo = bound->lo;
  } else if (lt) {
    if (bound->hi == VR_MIN) {
      return;
    }
    fact.hi = bound->hi - 1;
  } else if (le) {
    fact.hi = bound->hi;
  }
  vr_intersect(r, &fact);
}

static const char *vr_negate_relation(const char *op) {
  if (strcmp(op, ">") == 0) return "<=";
  if (strcmp(op, ">=") == 0) return "<";
  if (strcmp(op, "<") == 0) return ">=";
  if (strcmp(op, "<=") == 0) return ">";
  if (strcmp(op, "==") == 0) return "!=";
  if (strcmp(op, "!=") == 0) return "==";
  return NULL;
}

#define VR_LABEL_TRANSPARENT ((size_t)-2)

static size_t vr_label_entry_branch(IRValueRangeCtx *ctx, size_t label_index) {
  const IRFunction *fn = ctx->function;
  const char *label = fn->instructions[label_index].text;
  if (!label) {
    return (size_t)-1;
  }

  const IROperand *memo = ir_temp_value_map_lookup(&ctx->label_guard, label);
  if (memo && memo->kind == IR_OPERAND_INT) {
    if (memo->int_value == 0) {
      return (size_t)-1;
    }
    if (memo->int_value < 0) {
      return VR_LABEL_TRANSPARENT;
    }
    return (size_t)(memo->int_value - 1);
  }

  size_t result = (size_t)-1;
  int transparent = 0;
  int fallthrough_reaches = 1;
  for (size_t i = label_index; i > 0;) {
    i--;
    const IRInstruction *prev = &fn->instructions[i];
    if (prev->op == IR_OP_NOP) {
      continue;
    }
    fallthrough_reaches = !(prev->op == IR_OP_JUMP || prev->op == IR_OP_RETURN);
    break;
  }

  const IROperand *refs = ir_temp_value_map_lookup(&ctx->label_refs, label);
  if (fallthrough_reaches) {
    transparent = refs == NULL;
  } else if (refs && refs->kind == IR_OPERAND_INT && refs->int_value > 0) {
    result = (size_t)(refs->int_value - 1);
  }

  IROperand value = ir_operand_int(
      transparent ? -1 : (result == (size_t)-1 ? 0 : (long long)result + 1));
  ir_temp_value_map_set(&ctx->label_guard, label, &value);
  return transparent ? VR_LABEL_TRANSPARENT : result;
}

static void vr_apply_branch_fact(IRValueRangeCtx *ctx, size_t branch_index,
                                 const char *symbol, int arrived, int depth,
                                 IRIntRange *r) {
  const IRFunction *fn = ctx->function;
  const IRInstruction *branch = &fn->instructions[branch_index];
  if (branch->lhs.kind != IR_OPERAND_TEMP || !branch->lhs.name) {
    return;
  }

  size_t producer_index = 0;
  if (!vr_find_block_writer(fn, branch_index, IR_OPERAND_TEMP, branch->lhs.name,
                            0, &producer_index)) {
    return;
  }
  const IRInstruction *cmp = &fn->instructions[producer_index];
  if (cmp->op != IR_OP_BINARY || cmp->is_float || cmp->is_unsigned ||
      !cmp->text) {
    return;
  }
  for (size_t i = producer_index + 1; i < branch_index; i++) {
    const IRInstruction *in = &fn->instructions[i];
    if (ir_instruction_writes_destination(in) &&
        ir_operand_is_symbol(&in->dest) && symbol &&
        strcmp(in->dest.name, symbol) == 0) {
      return;
    }
  }

  const char *op = arrived ? vr_negate_relation(cmp->text) : cmp->text;
  if (!op) {
    return;
  }

  const IROperand *other = NULL;
  int symbol_on_left = 0;
  if (ir_operand_is_symbol_named(&cmp->lhs, symbol)) {
    other = &cmp->rhs;
    symbol_on_left = 1;
  } else if (ir_operand_is_symbol_named(&cmp->rhs, symbol)) {
    other = &cmp->lhs;
  } else {
    return;
  }

  IRIntRange bound;
  vr_operand_range(ctx, producer_index, other, depth + 1, &bound);
  if (vr_is_full(&bound)) {
    return;
  }
  vr_apply_relation(op, symbol_on_left, &bound, r);
}

static void vr_apply_guards(IRValueRangeCtx *ctx, size_t at, const char *symbol,
                            int depth, IRIntRange *r) {
  const IRFunction *fn = ctx->function;
  int private_local = vr_symbol_is_private(ctx, symbol);
  size_t scanned = 0;

  for (size_t i = at; i > 0 && scanned < VR_MAX_SCAN;) {
    i--;
    const IRInstruction *in = &fn->instructions[i];
    if (in->op == IR_OP_NOP) {
      continue;
    }
    scanned++;
    if (in->op == IR_OP_LABEL) {
      size_t entry = vr_label_entry_branch(ctx, i);
      if (entry == VR_LABEL_TRANSPARENT) {
        continue;
      }
      if (entry == (size_t)-1) {
        return;
      }
      vr_apply_branch_fact(ctx, entry, symbol, 1, depth, r);
      i = entry;
      continue;
    }
    if (ir_instruction_writes_destination(in) &&
        ir_operand_is_symbol(&in->dest) && symbol &&
        strcmp(in->dest.name, symbol) == 0) {
      return;
    }
    if (!private_local && vr_clobbers_memory(in)) {
      return;
    }
    if (in->op == IR_OP_BRANCH_ZERO) {
      vr_apply_branch_fact(ctx, i, symbol, 0, depth, r);
    }
  }
}

#define VR_IV_MAX_STEP (1ll << 20)

static int vr_monotone_write(const IRInstruction *in, const char *symbol,
                             int *saw_init) {
  if (!ir_instruction_writes_destination(in) ||
      in->dest.kind != IR_OPERAND_SYMBOL || !in->dest.name ||
      strcmp(in->dest.name, symbol) != 0) {
    return 1;
  }
  if (in->is_float) {
    return 0;
  }
  if (in->op == IR_OP_ASSIGN && in->lhs.kind == IR_OPERAND_INT &&
      in->lhs.int_value >= 0) {
    *saw_init = 1;
    return 1;
  }
  if (in->op == IR_OP_BINARY && in->text && strcmp(in->text, "+") == 0 &&
      ir_operand_is_symbol_named(&in->lhs, symbol) &&
      in->rhs.kind == IR_OPERAND_INT && in->rhs.int_value > 0 &&
      in->rhs.int_value <= VR_IV_MAX_STEP) {
    return 1;
  }
  return 0;
}

static int vr_monotone_scan(const IRFunction *fn, const char *symbol) {
  int saw_init = 0;
  for (size_t i = 0; i < fn->instruction_count; i++) {
    if (!vr_monotone_write(&fn->instructions[i], symbol, &saw_init)) {
      return 0;
    }
  }
  return saw_init;
}

static int vr_symbol_sites_build(IRValueRangeCtx *ctx) {
  const IRFunction *fn = ctx->function;
  size_t candidates = 0;
  size_t groups = 0;
  for (size_t i = 0; i < fn->instruction_count; i++) {
    const IRInstruction *in = &fn->instructions[i];
    if (in->dest.kind == IR_OPERAND_SYMBOL && in->dest.name) {
      candidates++;
    }
  }
  if (!ir_name_index_init(&ctx->symbol_groups, candidates)) {
    return 0;
  }
  ctx->symbol_sites_built = 1;
  ctx->symbol_sites =
      (size_t *)malloc((candidates ? candidates : 1) * sizeof(size_t));
  ctx->symbol_group_start =
      (size_t *)calloc(candidates + 2, sizeof(size_t));
  ctx->symbol_names =
      (char **)malloc((candidates ? candidates : 1) * sizeof(char *));
  size_t *fill = (size_t *)calloc(candidates + 1, sizeof(size_t));
  if (!ctx->symbol_sites || !ctx->symbol_group_start || !ctx->symbol_names ||
      !fill) {
    free(fill);
    return 0;
  }
  for (size_t i = 0; i < fn->instruction_count; i++) {
    const IRInstruction *in = &fn->instructions[i];
    size_t group = 0;
    if (in->dest.kind != IR_OPERAND_SYMBOL || !in->dest.name) {
      continue;
    }
    if (!ir_name_index_find(&ctx->symbol_groups, in->dest.name, &group)) {
      char *owned = mettle_strdup(in->dest.name);
      if (!owned) {
        free(fill);
        return 0;
      }
      ctx->symbol_names[ctx->symbol_name_count++] = owned;
      group = groups++;
      ir_name_index_insert(&ctx->symbol_groups, owned, group);
    }
    ctx->symbol_group_start[group + 1]++;
  }
  for (size_t g = 0; g < groups; g++) {
    ctx->symbol_group_start[g + 1] += ctx->symbol_group_start[g];
  }
  for (size_t i = 0; i < fn->instruction_count; i++) {
    const IRInstruction *in = &fn->instructions[i];
    size_t group = 0;
    if (in->dest.kind != IR_OPERAND_SYMBOL || !in->dest.name ||
        !ir_name_index_find(&ctx->symbol_groups, in->dest.name, &group)) {
      continue;
    }
    ctx->symbol_sites[ctx->symbol_group_start[group] + fill[group]++] = i;
  }
  free(fill);
  ctx->symbol_sites_insns = fn->instruction_count;
  return 1;
}

static int vr_symbol_sites_ready(IRValueRangeCtx *ctx) {
  if (ctx->symbol_sites_built &&
      ctx->symbol_sites_insns == ctx->function->instruction_count) {
    return ctx->symbol_sites != NULL && ctx->symbol_group_start != NULL &&
           ctx->symbol_names != NULL;
  }
  vr_symbol_sites_drop(ctx);
  return vr_symbol_sites_build(ctx);
}

static int vr_monotone_from_sites(const IRValueRangeCtx *ctx,
                                  const char *symbol) {
  size_t group = 0;
  int saw_init = 0;
  if (!ir_name_index_find(&ctx->symbol_groups, symbol, &group)) {
    return 0;
  }
  for (size_t p = ctx->symbol_group_start[group];
       p < ctx->symbol_group_start[group + 1]; p++) {
    if (!vr_monotone_write(&ctx->function->instructions[ctx->symbol_sites[p]],
                           symbol, &saw_init)) {
      return 0;
    }
  }
  return saw_init;
}

static int vr_symbol_is_monotone_counter(IRValueRangeCtx *ctx,
                                         const char *symbol) {
  if (!symbol) {
    return 0;
  }
  const IROperand *memo = ir_temp_value_map_lookup(&ctx->monotone, symbol);
  if (memo && memo->kind == IR_OPERAND_INT) {
    return memo->int_value != 0;
  }

  const IROperand *enc = ir_temp_value_map_lookup(&ctx->decl_types, symbol);
  int result = 0;
  if (enc && enc->kind == IR_OPERAND_INT && VR_TYPE_BITS(enc->int_value) == 64 &&
      !VR_TYPE_UNSIGNED(enc->int_value)) {
    if (vr_symbol_sites_ready(ctx)) {
      result = vr_monotone_from_sites(ctx, symbol);
    } else {
      result = vr_monotone_scan(ctx->function, symbol);
    }
    if (result && vr_symbol_address_taken(ctx, symbol)) {
      result = 0;
    }
  }

  IROperand value = ir_operand_int(result ? 1 : 0);
  ir_temp_value_map_set(&ctx->monotone, symbol, &value);
  return result;
}

static const IRInstruction *vr_counter_next_real(const IRFunction *fn,
                                                 size_t at) {
  for (size_t i = at + 1; i < fn->instruction_count; i++) {
    if (fn->instructions[i].op != IR_OP_NOP) {
      return &fn->instructions[i];
    }
  }
  return NULL;
}

static const IRInstruction *vr_sole_writer(IRValueRangeCtx *ctx,
                                           const IROperand *operand) {
  const IRFunction *fn = ctx->function;
  const IRInstruction *found = NULL;
  size_t group = 0;

  if (!operand->name) {
    return NULL;
  }
  if (operand->kind == IR_OPERAND_TEMP) {
    const IROperand *only =
        ir_temp_value_map_lookup(&ctx->unique_def, operand->name);
    return only && only->kind == IR_OPERAND_INT && only->int_value > 0
               ? &fn->instructions[only->int_value - 1]
               : NULL;
  }
  if (operand->kind != IR_OPERAND_SYMBOL || !vr_symbol_sites_ready(ctx) ||
      !ir_name_index_find(&ctx->symbol_groups, operand->name, &group)) {
    return NULL;
  }
  for (size_t p = ctx->symbol_group_start[group];
       p < ctx->symbol_group_start[group + 1]; p++) {
    const IRInstruction *in = &fn->instructions[ctx->symbol_sites[p]];
    if (in->op == IR_OP_DECLARE_LOCAL ||
        !ir_instruction_writes_destination(in)) {
      continue;
    }
    if (found) {
      return NULL;
    }
    found = in;
  }
  return found;
}

static int vr_counter_step_of(IRValueRangeCtx *ctx, const IRInstruction *write,
                              const char *symbol, long long *step) {
  const IRInstruction *add = write;

  if (write->op == IR_OP_ASSIGN) {
    add = vr_sole_writer(ctx, &write->lhs);
  }
  if (!add || add->op != IR_OP_BINARY || !add->text ||
      strcmp(add->text, "+") != 0 || add->rhs.kind != IR_OPERAND_INT ||
      add->rhs.int_value <= 0 || add->lhs.kind != IR_OPERAND_SYMBOL ||
      strcmp(add->lhs.name, symbol) != 0) {
    return 0;
  }
  *step = add->rhs.int_value;
  return 1;
}

static int vr_counter_init_of(IRValueRangeCtx *ctx, const IRInstruction *write,
                              long long *init) {
  if (write->op != IR_OP_ASSIGN) {
    return 0;
  }
  if (write->lhs.kind == IR_OPERAND_INT) {
    *init = write->lhs.int_value;
    return 1;
  }
  const IRInstruction *def = vr_sole_writer(ctx, &write->lhs);
  if (!def || def->op != IR_OP_ASSIGN || def->lhs.kind != IR_OPERAND_INT) {
    return 0;
  }
  *init = def->lhs.int_value;
  return 1;
}

static int vr_counter_guard_limit(const IRFunction *fn, const char *header,
                                  const char *symbol, long long *limit) {
  for (size_t i = 0; i < fn->instruction_count; i++) {
    const IRInstruction *label = &fn->instructions[i];
    const IRInstruction *compare;
    const IRInstruction *branch;

    if (label->op != IR_OP_LABEL || !label->text ||
        strcmp(label->text, header) != 0) {
      continue;
    }
    compare = vr_counter_next_real(fn, i);
    branch = compare ? vr_counter_next_real(
                           fn, (size_t)(compare - fn->instructions))
                     : NULL;
    if (!branch || compare->op != IR_OP_BINARY || !compare->text ||
        compare->lhs.kind != IR_OPERAND_SYMBOL ||
        strcmp(compare->lhs.name, symbol) != 0 ||
        compare->rhs.kind != IR_OPERAND_INT ||
        branch->op != IR_OP_BRANCH_ZERO ||
        !ir_operand_same(&branch->lhs, &compare->dest)) {
      return 0;
    }
    if (strcmp(compare->text, "<") == 0) {
      *limit = compare->rhs.int_value - 1;
      return 1;
    }
    if (strcmp(compare->text, "<=") == 0) {
      *limit = compare->rhs.int_value;
      return 1;
    }
    return 0;
  }
  return 0;
}

static int vr_counter_bounds_scan(IRValueRangeCtx *ctx, const char *symbol,
                                  IRIntRange *bounds) {
  const IRFunction *fn = ctx->function;
  const IRInstruction *writes[2];
  const IRInstruction *jump;
  size_t count = 0;
  long long init = 0;
  long long step = 0;
  long long limit = 0;
  size_t group = 0;

  if (!vr_symbol_sites_ready(ctx) ||
      !ir_name_index_find(&ctx->symbol_groups, symbol, &group)) {
    return 0;
  }
  for (size_t p = ctx->symbol_group_start[group];
       p < ctx->symbol_group_start[group + 1]; p++) {
    const IRInstruction *in = &fn->instructions[ctx->symbol_sites[p]];
    if (in->op == IR_OP_DECLARE_LOCAL ||
        !ir_instruction_writes_destination(in)) {
      continue;
    }
    if (count == 2) {
      return 0;
    }
    writes[count++] = in;
  }
  if (count != 2 || vr_symbol_address_taken(ctx, symbol) ||
      !vr_counter_init_of(ctx, writes[0], &init) ||
      !vr_counter_step_of(ctx, writes[1], symbol, &step)) {
    return 0;
  }
  jump = vr_counter_next_real(fn, (size_t)(writes[1] - fn->instructions));
  if (!jump || jump->op != IR_OP_JUMP || !jump->text ||
      !vr_counter_guard_limit(fn, jump->text, symbol, &limit) ||
      limit > 2147483647LL - step || init > limit) {
    return 0;
  }
  bounds->lo = init;
  bounds->hi = limit + step;
  return 1;
}

static int vr_counter_bounds(IRValueRangeCtx *ctx, const char *symbol,
                             IRIntRange *bounds) {
  const IROperand *lo = ir_temp_value_map_lookup(&ctx->counter_lo, symbol);
  const IROperand *hi = ir_temp_value_map_lookup(&ctx->counter_hi, symbol);
  IROperand lo_value;
  IROperand hi_value;
  int found;

  if (lo && hi && lo->kind == IR_OPERAND_INT && hi->kind == IR_OPERAND_INT) {
    bounds->lo = lo->int_value;
    bounds->hi = hi->int_value;
    return bounds->lo <= bounds->hi;
  }
  found = vr_counter_bounds_scan(ctx, symbol, bounds);
  lo_value = ir_operand_int(found ? bounds->lo : 1);
  hi_value = ir_operand_int(found ? bounds->hi : 0);
  ir_temp_value_map_set(&ctx->counter_lo, symbol, &lo_value);
  ir_temp_value_map_set(&ctx->counter_hi, symbol, &hi_value);
  return found;
}

static int vr_add_ok(long long a, long long b, long long *out) {
  if ((b > 0 && a > VR_MAX - b) || (b < 0 && a < VR_MIN - b)) {
    return 0;
  }
  *out = a + b;
  return 1;
}

static int vr_mul_nonneg_ok(long long a, long long b, long long *out) {
  if (a < 0 || b < 0) {
    return 0;
  }
  if (a != 0 && b > VR_MAX / a) {
    return 0;
  }
  *out = a * b;
  return 1;
}

static void vr_normalize(IRIntRange *r) {
  if (r->lo > r->hi) {
    vr_full(r);
  }
}

static int vr_is_comparison(const char *op) {
  return strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 ||
         strcmp(op, "<") == 0 || strcmp(op, "<=") == 0 ||
         strcmp(op, ">") == 0 || strcmp(op, ">=") == 0 ||
         strcmp(op, "&&") == 0 || strcmp(op, "||") == 0;
}

static unsigned long long vr_cover_mask(long long hi) {
  unsigned long long m = (unsigned long long)hi;
  m |= m >> 1;
  m |= m >> 2;
  m |= m >> 4;
  m |= m >> 8;
  m |= m >> 16;
  m |= m >> 32;
  return m;
}

static void vr_binary_range(IRValueRangeCtx *ctx, size_t index,
                            const IRInstruction *in, int depth,
                            IRIntRange *out) {
  const char *op = in->text;
  vr_full(out);
  if (!op) {
    return;
  }
  if (vr_is_comparison(op)) {
    out->lo = 0;
    out->hi = 1;
    return;
  }

  IRIntRange a, b;
  vr_operand_range(ctx, index, &in->lhs, depth + 1, &a);
  vr_operand_range(ctx, index, &in->rhs, depth + 1, &b);

  if (strcmp(op, "&") == 0) {
    if (a.lo >= 0) {
      out->lo = 0;
      out->hi = a.hi;
    }
    if (b.lo >= 0 && (out->lo < 0 || b.hi < out->hi)) {
      out->lo = 0;
      out->hi = b.hi;
    }
    return;
  }
  if (strcmp(op, "|") == 0 || strcmp(op, "^") == 0) {
    if (a.lo >= 0 && b.lo >= 0) {
      unsigned long long m = vr_cover_mask(a.hi) | vr_cover_mask(b.hi);
      out->lo = 0;
      out->hi = (long long)m;
    }
    return;
  }
  if (strcmp(op, ">>") == 0) {
    if (in->rhs.kind == IR_OPERAND_INT && in->rhs.int_value >= 0 &&
        in->rhs.int_value < 64 && a.lo >= 0) {
      out->lo = a.lo >> in->rhs.int_value;
      out->hi = a.hi >> in->rhs.int_value;
    }
    return;
  }
  if (strcmp(op, "<<") == 0) {
    if (in->rhs.kind == IR_OPERAND_INT && in->rhs.int_value >= 0 &&
        in->rhs.int_value < 63 && a.lo >= 0 &&
        a.hi <= (VR_MAX >> in->rhs.int_value)) {
      out->lo = a.lo << in->rhs.int_value;
      out->hi = a.hi << in->rhs.int_value;
    }
    return;
  }
  if (strcmp(op, "%") == 0) {
    if (a.lo >= 0 && b.lo > 0) {
      out->lo = 0;
      out->hi = b.hi - 1 < a.hi ? b.hi - 1 : a.hi;
    }
    return;
  }
  if (strcmp(op, "/") == 0) {
    if (a.lo >= 0 && b.lo > 0) {
      out->lo = a.lo / b.hi;
      out->hi = a.hi / b.lo;
    }
    return;
  }
  if (strcmp(op, "+") == 0) {
    long long lo = 0, hi = 0;
    if (vr_add_ok(a.lo, b.lo, &lo) && vr_add_ok(a.hi, b.hi, &hi)) {
      out->lo = lo;
      out->hi = hi;
    }
    return;
  }
  if (strcmp(op, "-") == 0) {
    long long lo = 0, hi = 0;
    if (b.lo != VR_MIN && b.hi != VR_MIN && vr_add_ok(a.lo, -b.hi, &lo) &&
        vr_add_ok(a.hi, -b.lo, &hi)) {
      out->lo = lo;
      out->hi = hi;
    }
    return;
  }
  if (strcmp(op, "*") == 0) {
    long long lo = 0, hi = 0;
    if (a.lo >= 0 && b.lo >= 0 && vr_mul_nonneg_ok(a.lo, b.lo, &lo) &&
        vr_mul_nonneg_ok(a.hi, b.hi, &hi)) {
      out->lo = lo;
      out->hi = hi;
      vr_normalize(out);
    }
    return;
  }
}

static void vr_instruction_range(IRValueRangeCtx *ctx, size_t index, int depth,
                                 IRIntRange *out) {
  const IRInstruction *in = &ctx->function->instructions[index];
  vr_full(out);
  if (in->is_float) {
    return;
  }

  switch (in->op) {
  case IR_OP_ASSIGN:
    vr_operand_range(ctx, index, &in->lhs, depth + 1, out);
    break;
  case IR_OP_LOAD:
    if (in->rhs.kind == IR_OPERAND_INT && in->rhs.int_value >= 1 &&
        in->rhs.int_value <= 4) {
      vr_of_int_type((int)(in->rhs.int_value * 8), in->is_unsigned, out);
    }
    break;
  case IR_OP_CAST: {
    int bits = 0, uns = 0;
    if (ir_int_type_name_info(in->text, &bits, &uns)) {
      IRIntRange target;
      vr_of_int_type(bits, uns, &target);
      vr_operand_range(ctx, index, &in->lhs, depth + 1, out);
      vr_narrow_to(out, &target);
    }
    break;
  }
  case IR_OP_BINARY:
    vr_binary_range(ctx, index, in, depth, out);
    break;
  case IR_OP_UNARY:
    if (in->text && strcmp(in->text, "!") == 0) {
      out->lo = 0;
      out->hi = 1;
    } else if (in->text && strcmp(in->text, "-") == 0) {
      IRIntRange a;
      vr_operand_range(ctx, index, &in->lhs, depth + 1, &a);
      if (a.lo > VR_MIN) {
        out->lo = -a.hi;
        out->hi = -a.lo;
      }
    }
    break;
  case IR_OP_SELECT: {
    if (in->argument_count != 1) {
      break;
    }
    IRIntRange then_r, else_r;
    vr_operand_range(ctx, index, &in->rhs, depth + 1, &then_r);
    vr_operand_range(ctx, index, &in->arguments[0], depth + 1, &else_r);
    out->lo = then_r.lo < else_r.lo ? then_r.lo : else_r.lo;
    out->hi = then_r.hi > else_r.hi ? then_r.hi : else_r.hi;
    break;
  }
  default:
    break;
  }

  if (ir_operand_is_symbol(&in->dest)) {
    IRIntRange declared;
    vr_declared_range(ctx, in->dest.name, &declared);
    vr_narrow_to(out, &declared);
  }
}

static void vr_operand_range(IRValueRangeCtx *ctx, size_t at,
                             const IROperand *operand, int depth,
                             IRIntRange *out) {
  vr_full(out);
  if (!operand || depth >= VR_MAX_DEPTH) {
    return;
  }

  if (operand->kind == IR_OPERAND_INT) {
    out->lo = operand->int_value;
    out->hi = operand->int_value;
    return;
  }

  if (operand->kind == IR_OPERAND_TEMP && operand->name) {
    size_t producer = 0;
    if (vr_find_block_writer(ctx->function, at, IR_OPERAND_TEMP, operand->name,
                             0, &producer)) {
      vr_instruction_range(ctx, producer, depth, out);
      return;
    }
    const IROperand *only =
        ir_temp_value_map_lookup(&ctx->unique_def, operand->name);
    if (only && only->kind == IR_OPERAND_INT && only->int_value > 0 &&
        (size_t)(only->int_value - 1) < at) {
      vr_instruction_range(ctx, (size_t)(only->int_value - 1), depth, out);
    }
    return;
  }

  if (operand->kind != IR_OPERAND_SYMBOL || !operand->name) {
    return;
  }

  vr_declared_range(ctx, operand->name, out);

  int private_local = vr_symbol_is_private(ctx, operand->name);
  size_t writer = 0;
  if (vr_find_block_writer(ctx->function, at, IR_OPERAND_SYMBOL, operand->name,
                           !private_local, &writer)) {
    IRIntRange defined;
    vr_instruction_range(ctx, writer, depth, &defined);
    vr_intersect(out, &defined);
  }

  if (out->lo < 0 && vr_symbol_is_monotone_counter(ctx, operand->name)) {
    IRIntRange nonneg = {0, VR_MAX};
    vr_intersect(out, &nonneg);
  }

  IRIntRange counter;
  if (vr_counter_bounds(ctx, operand->name, &counter)) {
    vr_intersect(out, &counter);
  }

  vr_apply_guards(ctx, at, operand->name, depth, out);
  vr_normalize(out);
}

void ir_value_range_of(IRValueRangeCtx *ctx, size_t at, const IROperand *operand,
                       IRIntRange *out) {
  vr_full(out);
  if (!ctx || !out || !vr_ctx_build(ctx) ||
      at > ctx->function->instruction_count) {
    return;
  }
  vr_operand_range(ctx, at, operand, 0, out);
  vr_normalize(out);
}

int ir_value_is_nonnegative(IRValueRangeCtx *ctx, size_t at,
                            const IROperand *operand) {
  IRIntRange r;
  ir_value_range_of(ctx, at, operand, &r);
  return r.lo >= 0;
}

static int vr_pow2_shift(long long value, long long *shift) {
  if (value <= 0) {
    return 0;
  }
  unsigned long long u = (unsigned long long)value;
  if ((u & (u - 1ull)) != 0ull) {
    return 0;
  }
  long long amount = 0;
  while (u > 1ull) {
    u >>= 1u;
    amount++;
  }
  *shift = amount;
  return 1;
}

static int vr_try_fold_comparison(IRValueRangeCtx *ctx, size_t at,
                                  IRInstruction *in, int *changed) {
  IRIntRange a, b;
  ir_value_range_of(ctx, at, &in->lhs, &a);
  if (a.lo < 0) {
    return 1;
  }
  ir_value_range_of(ctx, at, &in->rhs, &b);
  if (b.lo < 0) {
    return 1;
  }

  const char *op = in->text;
  int result = -1;
  if (strcmp(op, "<") == 0) {
    result = a.hi < b.lo ? 1 : (a.lo >= b.hi ? 0 : -1);
  } else if (strcmp(op, ">=") == 0) {
    result = a.hi < b.lo ? 0 : (a.lo >= b.hi ? 1 : -1);
  } else if (strcmp(op, ">") == 0) {
    result = a.lo > b.hi ? 1 : (a.hi <= b.lo ? 0 : -1);
  } else if (strcmp(op, "<=") == 0) {
    result = a.lo > b.hi ? 0 : (a.hi <= b.lo ? 1 : -1);
  } else if (strcmp(op, "==") == 0) {
    result = (a.hi < b.lo || a.lo > b.hi) ? 0 : -1;
  } else if (strcmp(op, "!=") == 0) {
    result = (a.hi < b.lo || a.lo > b.hi) ? 1 : -1;
  }
  if (result < 0) {
    return 1;
  }
  return ir_rewrite_to_assign_int(in, result, changed);
}

static int vr_try_resolve_branch(IRValueRangeCtx *ctx, size_t at,
                                 IRInstruction *in, int *changed) {
  IRIntRange a;

  if (in->op == IR_OP_BRANCH_ZERO) {
    if (in->lhs.kind == IR_OPERAND_INT) {
      return 1;
    }
    ir_value_range_of(ctx, at, &in->lhs, &a);
    if (a.lo > 0 || a.hi < 0) {
      ir_instruction_make_nop(in);
    } else if (a.lo == 0 && a.hi == 0) {
      ir_instruction_make_jump(in);
    } else {
      return 1;
    }
    if (changed) {
      *changed = 1;
    }
    return 1;
  }

  if (in->op == IR_OP_BRANCH_EQ) {
    if (in->lhs.kind == IR_OPERAND_INT && in->rhs.kind == IR_OPERAND_INT) {
      return 1;
    }
    IRIntRange b;
    ir_value_range_of(ctx, at, &in->lhs, &a);
    ir_value_range_of(ctx, at, &in->rhs, &b);
    if (a.hi < b.lo || b.hi < a.lo) {
      ir_instruction_make_nop(in);
      if (changed) {
        *changed = 1;
      }
    }
  }

  return 1;
}

static int vr_oracle_bounds_fit(long long lo, long long hi, int bits,
                                int is_unsigned);

/* A cast that re-canonicalizes a value already inside the target type is the
   identity, and in a scanner it sits in the loop's recurrence: the address of
   the next byte waits on it. */
static int vr_operand_is_unsigned(IRValueRangeCtx *ctx,
                                  const IROperand *operand) {
  int bits = 0;
  int uns = 0;
  if (operand->kind == IR_OPERAND_SYMBOL && operand->name) {
    const IROperand *enc =
        ir_temp_value_map_lookup(&ctx->decl_types, operand->name);
    if (!enc || enc->kind != IR_OPERAND_INT) {
      return -1;
    }
    return VR_TYPE_UNSIGNED(enc->int_value) ? 1 : 0;
  }
  if (operand->kind == IR_OPERAND_TEMP && operand->name) {
    const IROperand *only =
        ir_temp_value_map_lookup(&ctx->unique_def, operand->name);
    if (!only || only->kind != IR_OPERAND_INT || only->int_value <= 0 ||
        (size_t)(only->int_value - 1) >= ctx->function->instruction_count) {
      return -1;
    }
    const IRInstruction *def =
        &ctx->function->instructions[only->int_value - 1];
    if (!def->value_type ||
        !ir_int_type_name_info(def->value_type->name, &bits, &uns)) {
      return -1;
    }
    return uns;
  }
  return -1;
}

static int vr_reads_temp(const IRInstruction *use, const char *name) {
  if (ir_operand_is_temp_named(&use->lhs, name) ||
      ir_operand_is_temp_named(&use->rhs, name)) {
    return 1;
  }
  if (use->op == IR_OP_STORE && ir_operand_is_temp_named(&use->dest, name)) {
    return 1;
  }
  for (size_t a = 0; a < use->argument_count; a++) {
    if (ir_operand_is_temp_named(&use->arguments[a], name)) {
      return 1;
    }
  }
  return 0;
}

static int vr_use_ignores_signedness(const IRInstruction *use) {
  static const char *const kOps[] = {"+", "-", "*", "&", "|",
                                     "^", "<<", "==", "!="};
  switch (use->op) {
  case IR_OP_STORE:
  case IR_OP_CAST:
  case IR_OP_CALL:
  case IR_OP_RETURN:
    return 1;
  case IR_OP_BINARY:
    if (use->is_float || !use->text) {
      return 0;
    }
    for (size_t k = 0; k < sizeof(kOps) / sizeof(kOps[0]); k++) {
      if (strcmp(use->text, kOps[k]) == 0) {
        return 1;
      }
    }
    return 0;
  default:
    return 0;
  }
}

static int vr_readers_ignore_signedness(const IRFunction *function,
                                        const IROperand *dest) {
  if (dest->kind != IR_OPERAND_TEMP || !dest->name) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *use = &function->instructions[i];
    if (vr_reads_temp(use, dest->name) && !vr_use_ignores_signedness(use)) {
      return 0;
    }
  }
  return 1;
}

static int vr_cast_is_identity(IRValueRangeCtx *ctx, size_t at,
                               const IRInstruction *in) {
  int bits = 0;
  int uns = 0;
  IRIntRange a;
  if (in->op != IR_OP_CAST || in->is_float || !in->text ||
      in->dest.float_bits != 0 || in->lhs.float_bits != 0 ||
      (in->lhs.kind != IR_OPERAND_TEMP && in->lhs.kind != IR_OPERAND_SYMBOL)) {
    return 0;
  }
  if (!ir_int_type_name_info(in->text, &bits, &uns)) {
    return 0;
  }
  ir_value_range_of(ctx, at, &in->lhs, &a);
  if (!vr_oracle_bounds_fit(a.lo, a.hi, bits, uns)) {
    return 0;
  }
  if (vr_operand_is_unsigned(ctx, &in->lhs) == uns) {
    return 1;
  }
  return vr_readers_ignore_signedness(ctx->function, &in->dest);
}

int ir_drop_redundant_int_casts_pass(IRFunction *function, int *changed) {
  IRValueRangeCtx ranges;
  if (!function) {
    return 1;
  }
  ir_value_range_ctx_init(&ranges, function);
  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *in = &function->instructions[i];
    if (in->op != IR_OP_CAST || !vr_cast_is_identity(&ranges, i, in)) {
      continue;
    }
    if (!ir_rewrite_to_assign_operand(in, &in->lhs, changed)) {
      ir_value_range_ctx_destroy(&ranges);
      return 0;
    }
  }
  ir_value_range_ctx_destroy(&ranges);
  return 1;
}

int ir_value_range_simplify(IRValueRangeCtx *ctx, size_t at, IRInstruction *in,
                            int *changed) {
  if (!ctx || !in) {
    return 1;
  }
  if (in->op == IR_OP_BRANCH_ZERO || in->op == IR_OP_BRANCH_EQ) {
    return vr_try_resolve_branch(ctx, at, in, changed);
  }

  if (in->op != IR_OP_BINARY || in->is_float || !in->text) {
    return 1;
  }

  const char *op = in->text;
  int is_div = strcmp(op, "/") == 0;
  int is_mod = strcmp(op, "%") == 0;
  int is_and = strcmp(op, "&") == 0;

  if (is_div || is_mod) {
    if (in->rhs.kind != IR_OPERAND_INT || in->rhs.int_value <= 0) {
      return 1;
    }
    IRIntRange a;
    long long divisor = in->rhs.int_value;
    long long shift = 0;
    ir_value_range_of(ctx, at, &in->lhs, &a);
    if (a.lo < 0) {
      return 1;
    }
    if (a.hi < divisor) {
      if (is_div) {
        return ir_rewrite_to_assign_int(in, 0, changed);
      }
      return ir_rewrite_to_assign_operand(in, &in->lhs, changed);
    }
    if (!vr_pow2_shift(divisor, &shift)) {
      return 1;
    }
    {
      char *replacement = mettle_strdup(is_div ? ">>" : "&");
      if (!replacement) {
        return 0;
      }
      mettle_free_string(in->text);
      in->text = replacement;
      ir_operand_destroy(&in->rhs);
      in->rhs = ir_operand_int(is_div ? shift : divisor - 1);
      if (changed) {
        *changed = 1;
      }
    }
    return 1;
  }

  if (is_and) {
    const IROperand *value = NULL;
    long long mask = 0;
    if (in->rhs.kind == IR_OPERAND_INT && in->lhs.kind != IR_OPERAND_INT) {
      value = &in->lhs;
      mask = in->rhs.int_value;
    } else if (in->lhs.kind == IR_OPERAND_INT &&
               in->rhs.kind != IR_OPERAND_INT) {
      value = &in->rhs;
      mask = in->lhs.int_value;
    } else {
      return 1;
    }
    if (mask < 0) {
      return 1;
    }
    IRIntRange a;
    ir_value_range_of(ctx, at, value, &a);
    if (a.lo < 0) {
      return 1;
    }
    unsigned long long cover = vr_cover_mask(a.hi);
    if ((cover & (unsigned long long)mask) == cover) {
      return ir_rewrite_to_assign_operand(in, value, changed);
    }
    if ((cover & (unsigned long long)mask) == 0ull) {
      return ir_rewrite_to_assign_int(in, 0, changed);
    }
    return 1;
  }

  if (strcmp(op, ">>") == 0 && in->rhs.kind == IR_OPERAND_INT &&
      in->rhs.int_value > 0 && in->rhs.int_value < 64) {
    IRIntRange a;
    ir_value_range_of(ctx, at, &in->lhs, &a);
    if (a.lo >= 0 && (a.hi >> in->rhs.int_value) == 0) {
      return ir_rewrite_to_assign_int(in, 0, changed);
    }
    return 1;
  }

  if (vr_is_comparison(op) && strcmp(op, "&&") != 0 && strcmp(op, "||") != 0) {
    if (in->lhs.kind == IR_OPERAND_INT && in->rhs.kind == IR_OPERAND_INT) {
      return 1;
    }
    return vr_try_fold_comparison(ctx, at, in, changed);
  }

  return 1;
}

static const IROperand *vr_zero_test_operand(const IRInstruction *in) {
  if (in->op == IR_OP_BRANCH_ZERO && in->lhs.kind == IR_OPERAND_TEMP &&
      in->lhs.name) {
    return &in->lhs;
  }
  if (in->op != IR_OP_BINARY || in->is_float || !in->text ||
      (strcmp(in->text, "==") != 0 && strcmp(in->text, "!=") != 0)) {
    return NULL;
  }
  if (ir_operand_is_temp(&in->lhs) &&
      ir_operand_is_int_value(&in->rhs, 0)) {
    return &in->lhs;
  }
  if (ir_operand_is_temp(&in->rhs) &&
      ir_operand_is_int_value(&in->lhs, 0)) {
    return &in->rhs;
  }
  return NULL;
}

int ir_remainder_zero_test_to_mask_pass(IRFunction *function, int *changed) {
  if (!function) {
    return 0;
  }

  IRTempUseMap uses;
  if (!ir_temp_use_map_init(&uses)) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (!ir_collect_instruction_temp_uses(&uses, &function->instructions[i])) {
      ir_temp_use_map_destroy(&uses);
      return 0;
    }
  }

  for (size_t i = 0; i < function->instruction_count; i++) {
    const IROperand *tested = vr_zero_test_operand(&function->instructions[i]);
    if (!tested || ir_temp_use_map_get(&uses, tested->name) != 1) {
      continue;
    }

    size_t producer_index = 0;
    if (!vr_find_block_writer(function, i, IR_OPERAND_TEMP, tested->name, 0,
                              &producer_index)) {
      continue;
    }
    IRInstruction *producer = &function->instructions[producer_index];
    if (producer->op != IR_OP_BINARY || producer->is_float || !producer->text ||
        strcmp(producer->text, "%") != 0 ||
        producer->rhs.kind != IR_OPERAND_INT) {
      continue;
    }

    long long shift = 0;
    if (!vr_pow2_shift(producer->rhs.int_value, &shift) || shift < 1 ||
        shift > 62) {
      continue;
    }

    char *text = mettle_strdup("&");
    if (!text) {
      ir_temp_use_map_destroy(&uses);
      return 0;
    }
    mettle_free_string(producer->text);
    producer->text = text;
    producer->rhs.int_value = ((long long)1 << shift) - 1;
    if (changed) {
      *changed = 1;
    }
  }

  ir_temp_use_map_destroy(&uses);
  return 1;
}

void *ir_value_range_oracle_create(const IRFunction *function) {
  IRValueRangeCtx *ctx = (IRValueRangeCtx *)calloc(1, sizeof(IRValueRangeCtx));
  if (ctx) {
    ir_value_range_ctx_init(ctx, function);
  }
  return ctx;
}

void ir_value_range_oracle_destroy(void *oracle) {
  if (!oracle) {
    return;
  }
  ir_value_range_ctx_destroy((IRValueRangeCtx *)oracle);
  free(oracle);
}

static int vr_oracle_bounds_fit(long long lo, long long hi, int bits,
                                int is_unsigned) {
  if (bits <= 0 || bits >= 64) {
    return 0;
  }
  if (is_unsigned) {
    return lo >= 0 && hi <= (long long)((1ull << bits) - 1);
  }
  return lo >= -(1ll << (bits - 1)) && hi <= (1ll << (bits - 1)) - 1;
}

int ir_value_range_result_is_narrow(void *oracle, size_t at, int bits,
                                    int is_unsigned) {
  IRValueRangeCtx *ctx = (IRValueRangeCtx *)oracle;
  if (!ctx || !ctx->function || at >= ctx->function->instruction_count) {
    return 0;
  }
  const IRInstruction *in = &ctx->function->instructions[at];
  IRIntRange a, b;
  if (in->op == IR_OP_ASSIGN) {
    if (in->lhs.kind != IR_OPERAND_TEMP && in->lhs.kind != IR_OPERAND_SYMBOL &&
        in->lhs.kind != IR_OPERAND_INT) {
      return 0;
    }
    ir_value_range_of(ctx, at, &in->lhs, &a);
    return vr_oracle_bounds_fit(a.lo, a.hi, bits, is_unsigned);
  }
  if (in->op != IR_OP_BINARY || in->is_float || !in->text) {
    return 0;
  }
  int is_add = strcmp(in->text, "+") == 0;
  int is_sub = strcmp(in->text, "-") == 0;
  if (!is_add && !is_sub) {
    return 0;
  }
  ir_value_range_of(ctx, at, &in->lhs, &a);
  ir_value_range_of(ctx, at, &in->rhs, &b);
  const long long LIM = 1ll << 62;
  if (a.lo <= -LIM || a.hi >= LIM || b.lo <= -LIM || b.hi >= LIM) {
    return 0;
  }
  long long lo = is_add ? a.lo + b.lo : a.lo - b.hi;
  long long hi = is_add ? a.hi + b.hi : a.hi - b.lo;
  return vr_oracle_bounds_fit(lo, hi, bits, is_unsigned);
}
