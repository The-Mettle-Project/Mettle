enum { SAFETY_SHADOW_PARTIAL_WORD = 1 };

#define SAFETY_CACHE_TYPE ".safe_region_cache"

static int safety_register_cache_type(IRProgram *program) {
  if (ir_program_lookup_type(program, SAFETY_CACHE_TYPE)) {
    return 1;
  }
  const MtlcType *type = mtlc_type_array(mtlc_type_scalar(MTLC_TYPE_UINT64), 3);
  return type && ir_program_register_type(program, SAFETY_CACHE_TYPE,
                                          (MtlcType *)type);
}

typedef struct {
  size_t header_index;
  IROperand root;
  long long delta;
  int addressed;
  char temp[64];
} SafetyShadowSlot;

typedef struct {
  size_t header_index;
  const char *identity;
  int identity_kind;
  long long size;
  unsigned id;
} SafetyRegionCache;

typedef struct {
  SafetyShadowSlot *slots;
  size_t slot_count;
  size_t slot_capacity;
  SafetyRegionCache *caches;
  size_t cache_count;
  size_t cache_capacity;
  size_t *slot_of;
  size_t *cache_of;
  unsigned char *release_free;
  unsigned char *emits_here;
} SafetyInline;

static int safety_inline_prepare(SafetyInline *plan, const IRFunction *function,
                                 const SafetyLoopList *loops) {
  plan->slot_of = calloc(function->instruction_count + 1, sizeof(*plan->slot_of));
  plan->cache_of = calloc(function->instruction_count + 1, sizeof(*plan->cache_of));
  plan->emits_here = calloc(function->instruction_count + 1, 1);
  plan->release_free = calloc(loops->count + 1, 1);
  return plan->slot_of && plan->cache_of && plan->emits_here && plan->release_free;
}

static void safety_inline_release(SafetyInline *plan) {
  for (size_t s = 0; s < plan->slot_count; s++) {
    ir_operand_destroy(&plan->slots[s].root);
  }
  free(plan->slots);
  free(plan->caches);
  free(plan->slot_of);
  free(plan->cache_of);
  free(plan->release_free);
  free(plan->emits_here);
}

static const IROperand *safety_slot_root(const IRFunction *function, size_t before,
                                         const IROperand *slot, long long *delta,
                                         int *addressed, int depth) {
  if (slot->kind != IR_OPERAND_TEMP || !slot->name || depth > 8) {
    return slot;
  }
  const IRInstruction *producer =
      ir_find_temp_producer_before(function, before, slot->name);
  if (!producer || producer->is_float ||
      (producer->lhs.kind != IR_OPERAND_SYMBOL &&
       producer->lhs.kind != IR_OPERAND_TEMP)) {
    return slot;
  }
  size_t at = (size_t)(producer - function->instructions);
  int copies = producer->op == IR_OP_ASSIGN ||
      (producer->op == IR_OP_CAST &&
       (!producer->value_type || producer->value_type->size == 8));
  if (copies) {
    return safety_slot_root(function, at, &producer->lhs, delta, addressed,
                            depth + 1);
  }
  if (producer->op == IR_OP_BINARY && producer->text &&
      strcmp(producer->text, "+") == 0 && producer->rhs.kind == IR_OPERAND_INT) {
    *delta += producer->rhs.int_value;
    return safety_slot_root(function, at, &producer->lhs, delta, addressed,
                            depth + 1);
  }
  if (producer->op == IR_OP_ADDRESS_OF && producer->lhs.kind == IR_OPERAND_SYMBOL &&
      producer->lhs.name) {
    *addressed = 1;
    return &producer->lhs;
  }
  return slot;
}

static int safety_root_stays_put(const IRFunction *function,
                                 const IROperand *root) {
  if (root->kind == IR_OPERAND_TEMP) {
    return root->name != NULL;
  }
  return root->kind == IR_OPERAND_SYMBOL && root->name &&
         safety_name_is_functions_own(function, root->name) &&
         !safety_symbol_escapes(function, root->name);
}

static size_t safety_symbol_declared_at(const IRFunction *function,
                                        const char *name) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op == IR_OP_DECLARE_LOCAL && in->dest.kind == IR_OPERAND_SYMBOL &&
        in->dest.name && strcmp(in->dest.name, name) == 0) {
      return i;
    }
  }
  return 0;
}

static const SafetyLoopForm *safety_shadow_scope(const IRFunction *function,
                                                 const SafetyLoopList *loops,
                                                 size_t index,
                                                 const IROperand *root,
                                                 int addressed) {
  size_t declared = addressed ? safety_symbol_declared_at(function, root->name) : 0;
  for (size_t l = 0; l < loops->count; l++) {
    const SafetyLoopForm *loop = &loops->items[l];
    if (index <= loop->header_index || index >= loop->bounds.jump_index) {
      continue;
    }
    if (addressed ? declared < loop->header_index
                  : safety_operand_invariant_in(function, loop->header_index,
                                                loop->bounds.jump_index, root)) {
      return loop;
    }
  }
  return NULL;
}

static int safety_plan_shadow(const IRFunction *function,
                              const SafetyLoopList *loops, size_t index,
                              SafetyInline *plan) {
  const IRInstruction *in = &function->instructions[index];
  if (in->op != IR_OP_CALL || !in->text || in->argument_count != 3 ||
      !in->arguments || !in->dest.name ||
      strcmp(in->text, "mettle_safety_value_load") != 0 ||
      in->arguments[2].kind != IR_OPERAND_INT || in->arguments[2].int_value != 8) {
    return 1;
  }
  long long delta = 0;
  int addressed = 0;
  const IROperand *root =
      safety_slot_root(function, index, &in->arguments[0], &delta, &addressed, 0);
  const SafetyLoopForm *scope =
      safety_shadow_scope(function, loops, index, root, addressed);
  if (!scope || (!addressed && !safety_root_stays_put(function, root))) {
    return 1;
  }
  size_t found = plan->slot_count;
  for (size_t s = 0; s < plan->slot_count; s++) {
    if (plan->slots[s].header_index == scope->header_index &&
        plan->slots[s].delta == delta && plan->slots[s].addressed == addressed &&
        safety_operand_same(&plan->slots[s].root, root)) {
      found = s;
      break;
    }
  }
  if (found == plan->slot_count) {
    if (plan->slot_count == plan->slot_capacity) {
      size_t capacity = plan->slot_capacity ? plan->slot_capacity * 2 : 8;
      SafetyShadowSlot *grown = realloc(plan->slots, capacity * sizeof(*grown));
      if (!grown) return 0;
      plan->slots = grown;
      plan->slot_capacity = capacity;
    }
    SafetyShadowSlot *fresh = &plan->slots[plan->slot_count];
    memset(fresh, 0, sizeof(*fresh));
    if (!ir_operand_clone(root, &fresh->root)) return 0;
    fresh->header_index = scope->header_index;
    fresh->delta = delta;
    fresh->addressed = addressed;
    snprintf(fresh->temp, sizeof(fresh->temp), SAFETY_TEMP_PREFIX "sh%u",
             g_safety_next_id++);
    plan->slot_count++;
    plan->emits_here[scope->header_index] = 1;
  }
  plan->slot_of[index] = found + 1;
  return 1;
}

static const SafetyLoopForm *safety_cache_scope(const IRFunction *function,
                                                const SafetyLoopList *loops,
                                                size_t index,
                                                SafetyInline *plan) {
  for (size_t l = 0; l < loops->count; l++) {
    const SafetyLoopForm *loop = &loops->items[l];
    if (index <= loop->header_index || index >= loop->bounds.jump_index) {
      continue;
    }
    if (!plan->release_free[l]) {
      plan->release_free[l] =
          safety_body_has_no_calls(function, &loop->bounds) ? 1 : 2;
    }
    if (plan->release_free[l] == 1) {
      return loop;
    }
  }
  return NULL;
}

static int safety_plan_cache(const IRFunction *function,
                             const SafetyLoopList *loops, size_t index,
                             const SafetyAccess *access, SafetyInline *plan) {
  if (!access->identity) {
    return 1;
  }
  const IROperand *key = safety_identity_root(function, access->identity, index, 0);
  const SafetyLoopForm *scope = safety_cache_scope(function, loops, index, plan);
  if (!scope || !key || !key->name ||
      (key->kind != IR_OPERAND_SYMBOL && key->kind != IR_OPERAND_TEMP)) {
    return 1;
  }
  size_t found = plan->cache_count;
  for (size_t c = 0; c < plan->cache_count; c++) {
    if (plan->caches[c].header_index == scope->header_index &&
        plan->caches[c].identity_kind == (int)key->kind &&
        plan->caches[c].size == access->size &&
        strcmp(plan->caches[c].identity, key->name) == 0) {
      found = c;
      break;
    }
  }
  if (found == plan->cache_count) {
    if (plan->cache_count == plan->cache_capacity) {
      size_t capacity = plan->cache_capacity ? plan->cache_capacity * 2 : 8;
      SafetyRegionCache *grown = realloc(plan->caches, capacity * sizeof(*grown));
      if (!grown) return 0;
      plan->caches = grown;
      plan->cache_capacity = capacity;
    }
    SafetyRegionCache *fresh = &plan->caches[plan->cache_count++];
    fresh->header_index = scope->header_index;
    fresh->identity = key->name;
    fresh->identity_kind = key->kind;
    fresh->size = access->size;
    fresh->id = g_safety_next_id++;
    plan->emits_here[scope->header_index] = 1;
  }
  plan->cache_of[index] = found + 1;
  return 1;
}

static int safety_emit_assign(IRInstructionVector *out, SourceLocation location,
                              const IROperand *dest, const IROperand *value) {
  IRInstruction in = {0};
  in.op = IR_OP_ASSIGN;
  in.location = location;
  in.value_type = (MtlcType *)mtlc_type_scalar(MTLC_TYPE_UINT64);
  if (!ir_operand_clone(dest, &in.dest) || !ir_operand_clone(value, &in.lhs) ||
      !ir_instruction_vector_append_move(out, &in)) {
    ir_instruction_destroy_storage(&in);
    return 0;
  }
  return 1;
}

static int safety_emit_declare(IRInstructionVector *out, SourceLocation location,
                               const IROperand *symbol) {
  IRInstruction decl = {0};
  decl.op = IR_OP_DECLARE_LOCAL;
  decl.location = location;
  decl.text = mettle_strdup("uint64");
  decl.value_type = (MtlcType *)mtlc_type_scalar(MTLC_TYPE_UINT64);
  if (!decl.text || !ir_operand_clone(symbol, &decl.dest) ||
      !ir_instruction_vector_append_move(out, &decl)) {
    ir_instruction_destroy_storage(&decl);
    return 0;
  }
  return 1;
}

static int safety_emit_call_into(IRInstructionVector *out, SourceLocation location,
                                 const char *callee, const IROperand *arguments,
                                 size_t count, const IROperand *dest) {
  return safety_emit_call(out, location, callee, arguments, count) &&
         ir_operand_clone(dest, &out->items[out->count - 1].dest);
}


static int safety_emit_shadow_resolve(IRInstructionVector *out,
                                      const SafetyShadowSlot *slot,
                                      SourceLocation location) {
  char address[80];
  char taken[80];
  snprintf(address, sizeof(address), "%sa", slot->temp);
  snprintf(taken, sizeof(taken), "%sb", slot->temp);
  IROperand delta = ir_operand_int(slot->delta);
  IROperand displaced = ir_operand_temp(address);
  IROperand base = ir_operand_temp(taken);
  IROperand dest = ir_operand_temp(slot->temp);
  const IROperand *root = slot->addressed ? &base : &slot->root;
  int ok = displaced.name && base.name && dest.name;
  if (ok && slot->addressed) {
    IRInstruction take = {0};
    take.op = IR_OP_ADDRESS_OF;
    take.location = location;
    ok = ir_operand_clone(&base, &take.dest) &&
         ir_operand_clone(&slot->root, &take.lhs) &&
         ir_instruction_vector_append_move(out, &take);
    if (!ok) ir_instruction_destroy_storage(&take);
  }
  ok = ok &&
      (slot->delta == 0 ||
       safety_emit_binary(out, location, "+", address, root, &delta, 0)) &&
      safety_emit_call_into(out, location, "mettle_safety_shadow_slot",
                            slot->delta == 0 ? root : &displaced, 1, &dest);
  ir_operand_destroy(&displaced);
  ir_operand_destroy(&base);
  ir_operand_destroy(&dest);
  return ok;
}

static int safety_emit_shadow_load(IRInstructionVector *out, IRInstruction *call,
                                   const SafetyShadowSlot *slot) {
  unsigned id = g_safety_next_id++;
  SourceLocation location = call->location;
  char partial[64];
  char ok_label[64];
  snprintf(partial, sizeof(partial), SAFETY_TEMP_PREFIX "sw%u", id);
  snprintf(ok_label, sizeof(ok_label), "ir_safe_in_%u", id);
  IROperand marker = ir_operand_int(SAFETY_SHADOW_PARTIAL_WORD);
  IRInstruction load = {0};
  load.op = IR_OP_LOAD;
  load.location = location;
  load.value_type = (MtlcType *)mtlc_type_scalar(MTLC_TYPE_UINT64);
  load.is_unsigned = 1;
  load.lhs = ir_operand_temp(slot->temp);
  load.rhs = ir_operand_int(8);
  if (!load.lhs.name || !ir_operand_clone(&call->dest, &load.dest) ||
      !ir_instruction_vector_append_move(out, &load)) {
    ir_instruction_destroy_storage(&load);
    return 0;
  }
  return safety_emit_binary(out, location, "==", partial, &call->dest, &marker, 0) &&
         safety_emit_branch_zero(out, location, partial, ok_label) &&
         ir_instruction_vector_append_move(out, call) && safety_mark_cold(out) &&
         safety_emit_label(out, location, ok_label);
}

static int safety_emit_word_load(IRInstructionVector *out, SourceLocation location,
                                 const char *dest, const IROperand *address) {
  IRInstruction load = {0};
  load.op = IR_OP_LOAD;
  load.location = location;
  load.value_type = (MtlcType *)mtlc_type_scalar(MTLC_TYPE_UINT64);
  load.is_unsigned = 1;
  load.dest = ir_operand_temp(dest);
  load.rhs = ir_operand_int(8);
  if (!load.dest.name || !ir_operand_clone(address, &load.lhs) ||
      !ir_instruction_vector_append_move(out, &load)) {
    ir_instruction_destroy_storage(&load);
    return 0;
  }
  return 1;
}

static int safety_emit_word_store(IRInstructionVector *out, SourceLocation location,
                                  const IROperand *address, long long value) {
  IRInstruction store = {0};
  store.op = IR_OP_STORE;
  store.location = location;
  store.value_type = (MtlcType *)mtlc_type_scalar(MTLC_TYPE_UINT64);
  store.lhs = ir_operand_int(value);
  store.rhs = ir_operand_int(8);
  if (!ir_operand_clone(address, &store.dest) ||
      !ir_instruction_vector_append_move(out, &store)) {
    ir_instruction_destroy_storage(&store);
    return 0;
  }
  return 1;
}

static int safety_emit_cache_address(IRInstructionVector *out,
                                     SourceLocation location, unsigned cache_id,
                                     const char *pointer) {
  char record[64];
  snprintf(record, sizeof(record), SAFETY_TEMP_PREFIX "cache%u", cache_id);
  IRInstruction take = {0};
  take.op = IR_OP_ADDRESS_OF;
  take.location = location;
  take.dest = ir_operand_temp(pointer);
  take.lhs = ir_operand_symbol(record);
  if (!take.dest.name || !take.lhs.name ||
      !ir_instruction_vector_append_move(out, &take)) {
    ir_instruction_destroy_storage(&take);
    return 0;
  }
  return 1;
}

static int safety_emit_cache_init(IRInstructionVector *out,
                                  const SafetyRegionCache *cache,
                                  SourceLocation location) {
  char record[64];
  char pointer[64];
  snprintf(record, sizeof(record), SAFETY_TEMP_PREFIX "cache%u", cache->id);
  snprintf(pointer, sizeof(pointer), SAFETY_TEMP_PREFIX "cp%u", g_safety_next_id++);
  IROperand address = ir_operand_temp(pointer);
  IRInstruction decl = {0};
  decl.op = IR_OP_DECLARE_LOCAL;
  decl.location = location;
  decl.value_type = ir_program_lookup_type(g_safety_program, SAFETY_CACHE_TYPE);
  decl.text = mettle_strdup(SAFETY_CACHE_TYPE);
  decl.dest = ir_operand_symbol(record);
  int ok = decl.value_type && decl.text && decl.dest.name && address.name &&
      ir_instruction_vector_append_move(out, &decl) &&
      safety_emit_cache_address(out, location, cache->id, pointer) &&
      safety_emit_word_store(out, location, &address, SAFETY_SHADOW_PARTIAL_WORD);
  if (!ok) {
    ir_instruction_destroy_storage(&decl);
  }
  ir_operand_destroy(&address);
  return ok;
}

static int safety_emit_cache_field(IRInstructionVector *out, SourceLocation location,
                                   const IROperand *cache, long long offset,
                                   const char *at, const char *value) {
  IROperand displacement = ir_operand_int(offset);
  IROperand address = ir_operand_temp(at);
  int ok = address.name &&
      safety_emit_binary(out, location, "+", at, cache, &displacement, 0) &&
      safety_emit_word_load(out, location, value, &address);
  ir_operand_destroy(&address);
  return ok;
}

static int safety_emit_cache_refill(IRInstructionVector *out,
                                    const SafetyAccess *access,
                                    const IROperand *cache) {
  IROperand filled[3] = {*cache, *access->identity, ir_operand_int(access->size)};
  return safety_expand_region(out, access) && safety_mark_cold(out) &&
         safety_emit_call(out, access->location, "mettle_safety_cache_fill",
                          filled, 3);
}

static int safety_emit_cache_check(IRInstructionVector *out,
                                   const SafetyAccess *access, unsigned cache_id) {
  enum { CACHE, ADDRESS, HELD, SAME, LOW_AT, LOW, REACH, LIMIT_AT, LIMIT, PAST,
         NAMES };
  static const char *const tags[NAMES] = {"cp", "ca", "ch", "ce", "cq",
                                          "cl", "cr", "cw", "cm", "cx"};
  unsigned id = g_safety_next_id++;
  char names[NAMES][64];
  char ok_label[64];
  char slow_label[64];
  IROperand temps[NAMES];
  for (size_t n = 0; n < NAMES; n++) {
    snprintf(names[n], sizeof(names[n]), SAFETY_TEMP_PREFIX "%s%u", tags[n], id);
    temps[n] = ir_operand_temp(names[n]);
  }
  snprintf(ok_label, sizeof(ok_label), "ir_safe_in_%u", id);
  snprintf(slow_label, sizeof(slow_label), "ir_safe_out_%u", id);
  int offset_free = !access->offset || access->offset->kind == IR_OPERAND_NONE ||
      (access->offset->kind == IR_OPERAND_INT && access->offset->int_value == 0);
  const IROperand *address = offset_free ? access->base : &temps[ADDRESS];
  SourceLocation at = access->location;
  int ok = 1;
  for (size_t n = 0; n < NAMES; n++) ok = ok && temps[n].name;
  ok = ok &&
      (offset_free || safety_emit_binary(out, at, "+", names[ADDRESS],
                                         access->base, access->offset, 0)) &&
      safety_emit_cache_address(out, at, cache_id, names[CACHE]) &&
      safety_emit_word_load(out, at, names[HELD], &temps[CACHE]) &&
      safety_emit_binary(out, at, "==", names[SAME], access->identity,
                         &temps[HELD], 0) &&
      safety_emit_branch_zero(out, at, names[SAME], slow_label) &&
      safety_emit_cache_field(out, at, &temps[CACHE], 8, names[LOW_AT],
                              names[LOW]) &&
      safety_emit_binary(out, at, "-", names[REACH], address, &temps[LOW], 0) &&
      safety_emit_cache_field(out, at, &temps[CACHE], 16, names[LIMIT_AT],
                              names[LIMIT]) &&
      safety_emit_binary(out, at, ">", names[PAST], &temps[REACH],
                         &temps[LIMIT], 1) &&
      safety_emit_branch_zero(out, at, names[PAST], ok_label) &&
      safety_emit_label(out, at, slow_label) &&
      safety_emit_cache_refill(out, access, &temps[CACHE]) &&
      safety_emit_label(out, at, ok_label);
  for (size_t n = 0; n < NAMES; n++) ir_operand_destroy(&temps[n]);
  return ok;
}

static int safety_emit_inline_setup(IRInstructionVector *out,
                                    const SafetyInline *plan, size_t header_index,
                                    SourceLocation location) {
  if (!plan->emits_here[header_index]) {
    return 1;
  }
  for (size_t s = 0; s < plan->slot_count; s++) {
    if (plan->slots[s].header_index == header_index &&
        !safety_emit_shadow_resolve(out, &plan->slots[s], location)) {
      return 0;
    }
  }
  for (size_t c = 0; c < plan->cache_count; c++) {
    if (plan->caches[c].header_index == header_index &&
        !safety_emit_cache_init(out, &plan->caches[c], location)) {
      return 0;
    }
  }
  return 1;
}
