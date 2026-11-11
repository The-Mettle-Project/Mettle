typedef struct {
  const char *frame;
  const char *entry;
  size_t push_at;
  size_t enter_at;
  size_t count;
  unsigned first_id;
  const IROperand **passed;
  int valid;
} SafetyFrame;

typedef struct {
  const char *name;
  size_t frame;
  int entry;
} SafetyFrameName;

typedef struct {
  SafetyFrame *items;
  size_t count;
  size_t capacity;
  SafetyFrameName *names;
  size_t name_mask;
} SafetyFrames;

static int safety_call_named(const IRInstruction *in, const char *name,
                             size_t arity) {
  return in->op == IR_OP_CALL && in->text && in->arguments &&
         in->argument_count == arity && strcmp(in->text, name) == 0;
}

static const char *safety_frame_callee(const IRFunction *function, size_t at,
                                       const IROperand *operand) {
  if (operand->kind != IR_OPERAND_TEMP || !operand->name) return NULL;
  const IRInstruction *producer =
      ir_find_temp_producer_before(function, at, operand->name);
  if (!producer || producer->op != IR_OP_ADDRESS_OF ||
      producer->lhs.kind != IR_OPERAND_SYMBOL) return NULL;
  return producer->lhs.name;
}

static size_t safety_frame_find_enter(const IRFunction *function, size_t push_at,
                                      const char *callee) {
  for (size_t i = push_at + 1; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    switch (in->op) {
    case IR_OP_LABEL:
    case IR_OP_JUMP:
    case IR_OP_BRANCH_ZERO:
    case IR_OP_BRANCH_EQ:
    case IR_OP_RETURN:
    case IR_OP_CALL_INDIRECT:
    case IR_OP_INLINE_ASM:
    case IR_OP_GPU_LAUNCH:
      return 0;
    case IR_OP_CALL:
      if (!in->text || strncmp(in->text, "mettle_safety_", 14) != 0 ||
          safety_call_named(in, "mettle_safety_call_push", 2)) return 0;
      if (safety_call_named(in, "mettle_safety_call_enter", 1)) {
        const char *entered = safety_frame_callee(function, i, &in->arguments[0]);
        return entered && strcmp(entered, callee) == 0 ? i : 0;
      }
      break;
    default:
      break;
    }
  }
  return 0;
}

static SafetyFrameName *safety_frame_name_slot(const SafetyFrames *frames,
                                               const char *name) {
  size_t at = mettle_fnv1a_hash(name) & frames->name_mask;
  while (frames->names[at].name && strcmp(frames->names[at].name, name) != 0) {
    at = (at + 1) & frames->name_mask;
  }
  return &frames->names[at];
}

static const SafetyFrameName *safety_frame_named(const SafetyFrames *frames,
                                                 const IROperand *operand) {
  if (!frames->names || operand->kind != IR_OPERAND_TEMP || !operand->name) {
    return NULL;
  }
  const SafetyFrameName *slot = safety_frame_name_slot(frames, operand->name);
  return slot->name ? slot : NULL;
}

static int safety_frames_collect(const IRFunction *function, SafetyFrames *frames) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (!safety_call_named(in, "mettle_safety_call_push", 2) ||
        in->dest.kind != IR_OPERAND_TEMP || !in->dest.name ||
        in->arguments[1].kind != IR_OPERAND_INT || in->arguments[1].int_value < 0) {
      continue;
    }
    const char *callee = safety_frame_callee(function, i, &in->arguments[0]);
    size_t enter_at = callee ? safety_frame_find_enter(function, i, callee) : 0;
    if (!enter_at) continue;
    const IRInstruction *enter = &function->instructions[enter_at];
    if (enter->dest.kind != IR_OPERAND_TEMP || !enter->dest.name) continue;
    if (frames->count == frames->capacity) {
      size_t capacity = frames->capacity ? frames->capacity * 2 : 8;
      SafetyFrame *grown = realloc(frames->items, capacity * sizeof(*grown));
      if (!grown) return 0;
      frames->items = grown;
      frames->capacity = capacity;
    }
    SafetyFrame *frame = &frames->items[frames->count];
    memset(frame, 0, sizeof(*frame));
    frame->frame = in->dest.name;
    frame->entry = enter->dest.name;
    frame->push_at = i;
    frame->enter_at = enter_at;
    frame->count = (size_t)in->arguments[1].int_value;
    frame->passed = calloc(frame->count + 1, sizeof(*frame->passed));
    if (!frame->passed) return 0;
    frame->valid = 1;
    frames->count++;
  }
  if (!frames->count) return 1;
  size_t slots = 16;
  while (slots < frames->count * 4) slots *= 2;
  frames->names = calloc(slots, sizeof(*frames->names));
  if (!frames->names) return 0;
  frames->name_mask = slots - 1;
  for (size_t f = 0; f < frames->count; f++) {
    SafetyFrameName *pushed = safety_frame_name_slot(frames, frames->items[f].frame);
    if (pushed->name) {
      frames->items[f].valid = 0;
      frames->items[pushed->frame].valid = 0;
      continue;
    }
    pushed->name = frames->items[f].frame;
    pushed->frame = f;
    SafetyFrameName *entered = safety_frame_name_slot(frames, frames->items[f].entry);
    if (entered->name) {
      frames->items[f].valid = 0;
      frames->items[entered->frame].valid = 0;
      continue;
    }
    entered->name = frames->items[f].entry;
    entered->frame = f;
    entered->entry = 1;
  }
  return 1;
}

static int safety_frame_use_is_plain(SafetyFrame *frame, int entry,
                                     const IRInstruction *in, size_t at) {
  if (entry) {
    return (safety_call_named(in, "mettle_safety_call_param", 2) ||
            safety_call_named(in, "mettle_safety_call_param_value", 3)) ?
        in->arguments[1].kind == IR_OPERAND_INT && in->arguments[1].int_value >= 0 :
        safety_call_named(in, "mettle_safety_call_return", 2);
  }
  if (safety_call_named(in, "mettle_safety_call_pop", 1)) return 1;
  if (!safety_call_named(in, "mettle_safety_call_arg", 3) ||
      in->arguments[1].kind != IR_OPERAND_INT || in->arguments[1].int_value < 0 ||
      (size_t)in->arguments[1].int_value >= frame->count ||
      at <= frame->push_at || at >= frame->enter_at ||
      frame->passed[in->arguments[1].int_value]) {
    return 0;
  }
  frame->passed[in->arguments[1].int_value] = &in->arguments[2];
  return 1;
}

static void safety_frames_check_uses(const IRFunction *function,
                                     SafetyFrames *frames) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    const IROperand *fixed[3] = {&in->dest, &in->lhs, &in->rhs};
    for (size_t p = 0; p < 3 + in->argument_count; p++) {
      const IROperand *operand = p < 3 ? fixed[p] : &in->arguments[p - 3];
      const SafetyFrameName *named = safety_frame_named(frames, operand);
      if (!named) continue;
      SafetyFrame *frame = &frames->items[named->frame];
      if (p == 0) {
        if (i != (named->entry ? frame->enter_at : frame->push_at)) frame->valid = 0;
      } else if (p != 3 || !safety_frame_use_is_plain(frame, named->entry, in, i)) {
        frame->valid = 0;
      }
    }
  }
}

static int safety_frame_assign_named(IRInstructionVector *out,
                                     SourceLocation location,
                                     const IROperand *dest, const char *prefix,
                                     unsigned id, int symbol,
                                     const IROperand *value) {
  char name[64];
  snprintf(name, sizeof(name), "%s%u", prefix, id);
  IROperand named = symbol ? ir_operand_symbol(name) : ir_operand_temp(name);
  int ok = named.name != NULL &&
      safety_emit_assign(out, location, dest ? dest : &named, value ? value : &named);
  ir_operand_destroy(&named);
  return ok;
}

static int safety_frame_begin(IRInstructionVector *out, const SafetyFrame *frame,
                              SourceLocation location) {
  char name[64];
  snprintf(name, sizeof(name), ".safe_identity_%u", frame->first_id + (unsigned)frame->count);
  IROperand result = ir_operand_symbol(name);
  IROperand zero = ir_operand_int(0);
  int ok = result.name != NULL && safety_emit_declare(out, location, &result) &&
      safety_emit_assign(out, location, &result, &zero);
  ir_operand_destroy(&result);
  return ok;
}

static int safety_frame_rewrite_use(IRInstructionVector *out,
                                    const SafetyFrame *frame, int entry,
                                    const IRInstruction *in) {
  unsigned result = frame->first_id + (unsigned)frame->count;
  IROperand zero = ir_operand_int(0);
  if (!entry && safety_call_named(in, "mettle_safety_call_arg", 3)) {
    return safety_frame_assign_named(out, in->location, NULL, ".safe_origin_",
        frame->first_id + (unsigned)in->arguments[1].int_value, 0,
        &in->arguments[2]);
  }
  if (entry && safety_call_named(in, "mettle_safety_call_return", 2)) {
    return safety_frame_assign_named(out, in->location, NULL, ".safe_identity_",
        result, 1, &in->arguments[1]);
  }
  if (in->dest.kind == IR_OPERAND_NONE) return 1;
  if (!entry) {
    return safety_frame_assign_named(out, in->location, &in->dest,
        ".safe_identity_", result, 1, NULL);
  }
  size_t index = (size_t)in->arguments[1].int_value;
  if (index >= frame->count || !frame->passed[index]) {
    return safety_emit_assign(out, in->location, &in->dest, &zero);
  }
  return safety_frame_assign_named(out, in->location, &in->dest, ".safe_origin_",
      frame->first_id + (unsigned)index, 0, NULL);
}

static int safety_frames_rewrite(IRFunction *function, SafetyFrames *frames) {
  IRInstructionVector out = {0};
  if (!ir_instruction_vector_reserve(&out, function->instruction_count + 16)) {
    return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *in = &function->instructions[i];
    const SafetyFrameName *named = safety_frame_named(frames, &in->dest);
    const SafetyFrame *frame = named ? &frames->items[named->frame] : NULL;
    int handled = 0;
    int ok = 1;
    if (frame && frame->valid) {
      handled = 1;
      if (!named->entry) ok = safety_frame_begin(&out, frame, in->location);
    } else if (in->op == IR_OP_CALL && in->argument_count > 0) {
      named = safety_frame_named(frames, &in->arguments[0]);
      frame = named ? &frames->items[named->frame] : NULL;
      if (frame && frame->valid) {
        handled = 1;
        ok = safety_frame_rewrite_use(&out, frame, named->entry, in);
      }
    }
    if (!handled) ok = ir_instruction_vector_append_move(&out, in);
    if (!ok) {
      ir_instruction_vector_destroy(&out);
      return 0;
    }
  }
  if (!ir_function_replace_instructions(function, &out)) {
    ir_instruction_vector_destroy(&out);
    return 0;
  }
  ir_function_clear_cfg(function);
  return 1;
}

static int safety_elide_inlined_frames(IRFunction *function) {
  SafetyFrames frames = {0};
  int ok = safety_frames_collect(function, &frames);
  size_t valid = 0;
  if (ok && frames.count) {
    safety_frames_check_uses(function, &frames);
    for (size_t f = 0; f < frames.count; f++) {
      if (!frames.items[f].valid) continue;
      frames.items[f].first_id = g_safety_next_id;
      g_safety_next_id += (unsigned)frames.items[f].count + 1;
      valid++;
    }
    if (valid) ok = safety_frames_rewrite(function, &frames);
  }
  for (size_t f = 0; f < frames.count; f++) free(frames.items[f].passed);
  free(frames.items);
  free(frames.names);
  return ok;
}

int ir_safety_elide_inlined_frames(IRProgram *program) {
  if (!program) return 1;
  for (size_t f = 0; f < program->function_count; f++) {
    IRFunction *function = program->functions[f];
    if (function && !function->is_rule && !function->is_naked &&
        !safety_elide_inlined_frames(function)) return 0;
  }
  return 1;
}
