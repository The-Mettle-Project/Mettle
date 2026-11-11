enum {
  SAFETY_NUMBER_LEAF,
  SAFETY_NUMBER_CONST,
  SAFETY_NUMBER_ADD,
  SAFETY_NUMBER_MERGE,
  SAFETY_NUMBER_OP
};

enum {
  SAFETY_COVER_TERMS = 4,
  SAFETY_COVER_WINDOW = 64,
  SAFETY_COVER_DEPTH = 8
};

typedef struct {
  unsigned char kind;
  IROpcode op;
  const char *text;
  const MtlcType *type;
  unsigned flags;
  long long constant;
  unsigned left;
  unsigned right;
} SafetyNumber;

typedef struct {
  const char *name;
  int kind;
  unsigned number;
  unsigned run;
  unsigned epoch;
  int resident;
} SafetyNumbered;

typedef struct {
  unsigned terms[SAFETY_COVER_TERMS];
  size_t count;
  long long constant;
  int exact;
} SafetyReach;

typedef struct {
  unsigned identity;
  unsigned base;
  SafetyReach reach;
  long long high;
} SafetyCovered;

typedef struct {
  const IRFunction *function;
  SafetyNumber *numbers;
  size_t number_count;
  size_t number_capacity;
  SafetyNumbered *names;
  size_t name_mask;
  unsigned *expressions;
  unsigned *expression_runs;
  size_t expression_mask;
  IRNameMap addressed;
  IRNameMap owned;
  unsigned run;
  unsigned epoch;
  SafetyCovered covered[SAFETY_COVER_WINDOW];
  size_t covered_count;
  size_t covered_next;
} SafetyCover;

static unsigned safety_number_append(SafetyCover *cover, const SafetyNumber *number) {
  if (cover->number_count == cover->number_capacity) return 0;
  cover->numbers[cover->number_count] = *number;
  return (unsigned)cover->number_count++;
}

static unsigned safety_number_leaf(SafetyCover *cover) {
  SafetyNumber leaf = {0};
  leaf.kind = SAFETY_NUMBER_LEAF;
  return safety_number_append(cover, &leaf);
}

static int safety_number_matches(const SafetyNumber *a, const SafetyNumber *b) {
  if (a->kind != b->kind || a->op != b->op || a->type != b->type ||
      a->flags != b->flags || a->constant != b->constant ||
      a->left != b->left || a->right != b->right) return 0;
  if (!a->text || !b->text) return a->text == b->text;
  return strcmp(a->text, b->text) == 0;
}

static unsigned safety_number_intern(SafetyCover *cover, const SafetyNumber *wanted) {
  uint64_t hash = mettle_fnv1a_hash(wanted->text ? wanted->text : "");
  hash = (hash ^ wanted->kind) * UINT64_C(0x9E3779B97F4A7C15);
  hash = (hash ^ (uint64_t)wanted->op) * UINT64_C(0x9E3779B97F4A7C15);
  hash = (hash ^ (uint64_t)wanted->constant) * UINT64_C(0x9E3779B97F4A7C15);
  hash = (hash ^ wanted->left) * UINT64_C(0x9E3779B97F4A7C15);
  hash = (hash ^ wanted->right) * UINT64_C(0x9E3779B97F4A7C15);
  size_t at = (size_t)(hash >> 20) & cover->expression_mask;
  while (cover->expression_runs[at] == cover->run) {
    unsigned held = cover->expressions[at];
    if (safety_number_matches(&cover->numbers[held], wanted)) return held;
    at = (at + 1) & cover->expression_mask;
  }
  unsigned made = safety_number_append(cover, wanted);
  if (made) {
    cover->expressions[at] = made;
    cover->expression_runs[at] = cover->run;
  }
  return made;
}

static unsigned safety_number_const(SafetyCover *cover, long long value) {
  SafetyNumber constant = {0};
  constant.kind = SAFETY_NUMBER_CONST;
  constant.constant = value;
  return safety_number_intern(cover, &constant);
}

static SafetyNumbered *safety_numbered_slot(SafetyCover *cover,
                                            const IROperand *operand) {
  size_t at = mettle_fnv1a_hash(operand->name) & cover->name_mask;
  while (cover->names[at].name &&
         !(cover->names[at].kind == (int)operand->kind &&
           strcmp(cover->names[at].name, operand->name) == 0)) {
    at = (at + 1) & cover->name_mask;
  }
  SafetyNumbered *slot = &cover->names[at];
  if (!slot->name) {
    slot->name = operand->name;
    slot->kind = operand->kind;
    slot->resident = operand->kind == IR_OPERAND_SYMBOL &&
        (ir_name_map_lookup(&cover->addressed, operand->name) != NULL ||
         ir_name_map_lookup(&cover->owned, operand->name) == NULL);
  }
  return slot;
}

static unsigned safety_number_of(SafetyCover *cover, const IROperand *operand) {
  if (!operand || operand->kind == IR_OPERAND_NONE) {
    return safety_number_const(cover, 0);
  }
  if (operand->kind == IR_OPERAND_INT) {
    return safety_number_const(cover, operand->int_value);
  }
  if (!operand->name ||
      (operand->kind != IR_OPERAND_TEMP && operand->kind != IR_OPERAND_SYMBOL)) {
    return safety_number_leaf(cover);
  }
  SafetyNumbered *slot = safety_numbered_slot(cover, operand);
  if (slot->run != cover->run || (slot->resident && slot->epoch != cover->epoch)) {
    slot->number = safety_number_leaf(cover);
    slot->run = cover->run;
    slot->epoch = cover->epoch;
  }
  return slot->number;
}

static void safety_number_bind(SafetyCover *cover, const IROperand *dest,
                               unsigned number) {
  if (!dest->name ||
      (dest->kind != IR_OPERAND_TEMP && dest->kind != IR_OPERAND_SYMBOL)) return;
  SafetyNumbered *slot = safety_numbered_slot(cover, dest);
  slot->number = number;
  slot->run = cover->run;
  slot->epoch = cover->epoch;
}

static unsigned safety_number_pair(SafetyCover *cover, unsigned char kind,
                                   unsigned left, unsigned right) {
  SafetyNumber pair = {0};
  pair.kind = kind;
  pair.left = left;
  pair.right = right;
  return safety_number_intern(cover, &pair);
}

static unsigned safety_number_merged(SafetyCover *cover, const IRInstruction *in) {
  unsigned left = safety_number_of(cover, &in->arguments[0]);
  unsigned right = safety_number_of(cover, &in->arguments[1]);
  const SafetyNumber *a = &cover->numbers[left];
  const SafetyNumber *b = &cover->numbers[right];
  if (a->kind == SAFETY_NUMBER_CONST && a->constant == 0) return right;
  if (left == right || (b->kind == SAFETY_NUMBER_CONST && b->constant == 0)) return left;
  return safety_number_pair(cover, SAFETY_NUMBER_MERGE, left, right);
}

static unsigned safety_number_binary(SafetyCover *cover, const IRInstruction *in) {
  unsigned left = safety_number_of(cover, &in->lhs);
  unsigned right = safety_number_of(cover, &in->rhs);
  const SafetyNumber *b = &cover->numbers[right];
  if (!in->is_float && in->text && strcmp(in->text, "+") == 0) {
    return safety_number_pair(cover, SAFETY_NUMBER_ADD, left, right);
  }
  if (!in->is_float && in->text && strcmp(in->text, "-") == 0 &&
      b->kind == SAFETY_NUMBER_CONST && b->constant != LLONG_MIN) {
    return safety_number_pair(cover, SAFETY_NUMBER_ADD, left,
                              safety_number_const(cover, -b->constant));
  }
  SafetyNumber made = {0};
  made.kind = SAFETY_NUMBER_OP;
  made.op = in->op;
  made.text = in->text;
  made.type = in->value_type;
  made.flags = (in->is_float ? 1u : 0u) | (in->is_unsigned ? 2u : 0u) |
               ((unsigned)in->float_bits << 2);
  made.left = left;
  made.right = right;
  return safety_number_intern(cover, &made);
}

static unsigned safety_number_defined(SafetyCover *cover, const IRInstruction *in) {
  SafetyNumber made = {0};
  switch (in->op) {
  case IR_OP_ASSIGN:
    return safety_number_of(cover, &in->lhs);
  case IR_OP_BINARY:
    return safety_number_binary(cover, in);
  case IR_OP_CAST:
  case IR_OP_UNARY:
    made.kind = SAFETY_NUMBER_OP;
    made.op = in->op;
    made.text = in->text;
    made.type = in->value_type;
    made.flags = (in->is_float ? 1u : 0u) | (in->is_unsigned ? 2u : 0u) |
                 ((unsigned)in->float_bits << 2);
    made.left = safety_number_of(cover, &in->lhs);
    return safety_number_intern(cover, &made);
  case IR_OP_ADDRESS_OF:
    if (in->lhs.kind != IR_OPERAND_SYMBOL || !in->lhs.name) break;
    made.kind = SAFETY_NUMBER_OP;
    made.op = in->op;
    made.text = in->lhs.name;
    return safety_number_intern(cover, &made);
  case IR_OP_CALL:
    if (in->text && in->argument_count == 2 && in->arguments &&
        strcmp(in->text, "mettle_safety_merge_identity") == 0) {
      return safety_number_merged(cover, in);
    }
    break;
  default:
    break;
  }
  return safety_number_leaf(cover);
}

static void safety_reach_add(const SafetyCover *cover, unsigned number,
                             SafetyReach *reach, int depth) {
  const SafetyNumber *value = &cover->numbers[number];
  if (value->kind == SAFETY_NUMBER_CONST) {
    reach->constant = (long long)((unsigned long long)reach->constant +
                                  (unsigned long long)value->constant);
    return;
  }
  if (value->kind == SAFETY_NUMBER_ADD && depth < SAFETY_COVER_DEPTH) {
    safety_reach_add(cover, value->left, reach, depth + 1);
    safety_reach_add(cover, value->right, reach, depth + 1);
    return;
  }
  if (!number || reach->count == SAFETY_COVER_TERMS) {
    reach->exact = 0;
    return;
  }
  size_t at = reach->count++;
  while (at > 0 && reach->terms[at - 1] > number) {
    reach->terms[at] = reach->terms[at - 1];
    at--;
  }
  reach->terms[at] = number;
}

static int safety_reach_same_terms(const SafetyReach *a, const SafetyReach *b) {
  if (a->count != b->count) return 0;
  for (size_t t = 0; t < a->count; t++) {
    if (a->terms[t] != b->terms[t]) return 0;
  }
  return 1;
}

static int safety_identity_reaches(const SafetyCover *cover, unsigned later,
                                   unsigned earlier, int depth) {
  if (later == earlier) return 1;
  const SafetyNumber *value = &cover->numbers[later];
  return value->kind == SAFETY_NUMBER_MERGE && depth < SAFETY_COVER_DEPTH &&
         (safety_identity_reaches(cover, value->left, earlier, depth + 1) ||
          safety_identity_reaches(cover, value->right, earlier, depth + 1));
}

static int safety_cover_base_was_checked(const SafetyCover *cover, unsigned base) {
  SafetyReach reach = {0};
  reach.exact = 1;
  safety_reach_add(cover, base, &reach, 0);
  for (size_t c = 0; c < cover->covered_count; c++) {
    const SafetyCovered *held = &cover->covered[c];
    if (held->base == base) return 1;
    if (reach.exact && safety_reach_same_terms(&reach, &held->reach) &&
        held->reach.constant <= reach.constant && reach.constant < held->high) {
      return 1;
    }
  }
  return 0;
}

static int safety_cover_check(SafetyCover *cover, const SafetyAccess *access) {
  SafetyCovered made = {0};
  made.base = safety_number_of(cover, access->base);
  unsigned offset = safety_number_of(cover, access->offset);
  made.identity = access->identity ? safety_number_of(cover, access->identity) : 0;
  made.reach.exact = 1;
  safety_reach_add(cover, made.base, &made.reach, 0);
  safety_reach_add(cover, offset, &made.reach, 0);
  if (!made.base || !offset || (access->identity && !made.identity) ||
      !made.reach.exact || access->size <= 0 ||
      made.reach.constant > LLONG_MAX - access->size) {
    return 0;
  }
  made.high = made.reach.constant + access->size;
  for (size_t c = 0; c < cover->covered_count; c++) {
    const SafetyCovered *held = &cover->covered[c];
    if (safety_reach_same_terms(&made.reach, &held->reach) &&
        held->reach.constant <= made.reach.constant && made.high <= held->high &&
        safety_identity_reaches(cover, made.identity, held->identity, 0) &&
        safety_cover_base_was_checked(cover, made.base)) {
      return 1;
    }
  }
  cover->covered[cover->covered_next] = made;
  cover->covered_next = (cover->covered_next + 1) % SAFETY_COVER_WINDOW;
  if (cover->covered_count < SAFETY_COVER_WINDOW) cover->covered_count++;
  return 0;
}

static int safety_cover_leaves_memory_alone(const IRInstruction *in) {
  switch (in->op) {
  case IR_OP_ASSIGN:
  case IR_OP_ADDRESS_OF:
  case IR_OP_LOAD:
  case IR_OP_BINARY:
  case IR_OP_UNARY:
  case IR_OP_CAST:
  case IR_OP_SELECT:
  case IR_OP_ROTATE_ADD:
  case IR_OP_JUMP:
  case IR_OP_BRANCH_ZERO:
  case IR_OP_BRANCH_EQ:
  case IR_OP_RETURN:
  case IR_OP_PREFETCH:
    return 1;
  case IR_OP_CALL:
    return in->text && strncmp(in->text, "mettle_safety_", 14) == 0;
  default:
    return 0;
  }
}

static void safety_cover_begin_run(SafetyCover *cover) {
  cover->run++;
  cover->number_count = 1;
  cover->covered_count = 0;
  cover->covered_next = 0;
}

static int safety_cover_prepare(SafetyCover *cover, const IRFunction *function) {
  size_t operands = function->parameter_count + 8;
  for (size_t p = 0; p < function->parameter_count; p++) {
    const char *name = function->parameter_names[p];
    if (name && !ir_name_map_add(&cover->owned, name, name)) return 0;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    operands += 3 + in->argument_count;
    if (in->op == IR_OP_ADDRESS_OF && ir_operand_is_symbol(&in->lhs) &&
        !ir_name_map_add(&cover->addressed, in->lhs.name, in->lhs.name)) {
      return 0;
    }
    if (in->op == IR_OP_DECLARE_LOCAL && ir_operand_is_symbol(&in->dest) &&
        !ir_name_map_add(&cover->owned, in->dest.name, in->dest.name)) {
      return 0;
    }
  }
  size_t names = 16;
  while (names < operands * 2) names *= 2;
  cover->function = function;
  cover->number_capacity = operands * 2 + function->instruction_count * 2 + 16;
  size_t expressions = 16;
  while (expressions < cover->number_capacity * 2) expressions *= 2;
  cover->numbers = malloc(cover->number_capacity * sizeof(*cover->numbers));
  cover->names = calloc(names, sizeof(*cover->names));
  cover->expressions = malloc(expressions * sizeof(*cover->expressions));
  cover->expression_runs = calloc(expressions, sizeof(*cover->expression_runs));
  cover->name_mask = names - 1;
  cover->expression_mask = expressions - 1;
  if (!cover->numbers || !cover->names || !cover->expressions ||
      !cover->expression_runs) return 0;
  memset(&cover->numbers[0], 0, sizeof(cover->numbers[0]));
  return 1;
}

static void safety_cover_release(SafetyCover *cover) {
  free(cover->numbers);
  free(cover->names);
  free(cover->expressions);
  free(cover->expression_runs);
  ir_name_map_destroy(&cover->addressed);
  ir_name_map_destroy(&cover->owned);
}

static int safety_mark_covered(const IRFunction *function, unsigned char *outcome,
                               unsigned char covered) {
  SafetyCover *cover = calloc(1, sizeof(*cover));
  if (!cover) return 0;
  int ok = safety_cover_prepare(cover, function);
  if (ok) safety_cover_begin_run(cover);
  for (size_t i = 0; ok && i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op == IR_OP_NOP || in->op == IR_OP_DECLARE_LOCAL) continue;
    if (in->op == IR_OP_LABEL) {
      safety_cover_begin_run(cover);
      continue;
    }
    if (in->op == IR_OP_SAFETY_CHECK) {
      SafetyAccess access;
      if (!safety_read(in, &access)) {
        ok = 0;
      } else if (safety_cover_check(cover, &access)) {
        outcome[i] = covered;
      }
      continue;
    }
    if (safety_body_instruction_can_release(in)) {
      cover->covered_count = 0;
      cover->covered_next = 0;
    }
    if (!safety_cover_leaves_memory_alone(in)) cover->epoch++;
    if (ir_instruction_writes_destination(in)) {
      safety_number_bind(cover, &in->dest, safety_number_defined(cover, in));
    }
  }
  safety_cover_release(cover);
  free(cover);
  return ok;
}
