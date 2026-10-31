#include "ir_lowering_internal.h"
#include "frontend/mtlc_frontend.h"
#include <ctype.h>
#include <stdint.h>

#define IR_LIVE_CHECK_NAME "mettle_live_frame_check"
#define IR_LIVE_PUT_NAME "mettle_live_frame_put"
#define IR_LIVE_TAKE_NAME "mettle_live_frame_take"
#define IR_LIVE_ZERO_NAME "mettle_live_frame_zero"
#define IR_LIVE_ALLOC_NAME "mettle_live_frame_alloc"
#define IR_LIVE_END_NAME "mettle_live_frame_end"
#define IR_LIVE_QUIESCE_NAME "mettle_live_quiesce"
#define IR_LIVE_RESUME_INFIX "$resume_"
#define IR_LIVE_FRAME_SUFFIX "$live_frame"

typedef struct {
  const char **keys;
  char **values;
  size_t capacity;
  size_t count;
} IRLiveMap;

typedef struct {
  const char *name;
  char *key;
  const char *type_text;
  MtlcType *type;
  long long layout;
  long long size;
  int scalar;
  int float_bits;
  int is_unsigned;
  int address_taken;
  size_t declared_at;
  IROperand initial;
  const char *initial_global;
} IRLiveLocal;

typedef struct {
  IRLiveLocal *items;
  size_t count;
} IRLiveLocals;

typedef struct {
  size_t call_index;
  const char *resume_label;
  size_t local_count;
} IRLiveSite;

typedef struct {
  size_t block_count;
  size_t *rpo;
  size_t *rpo_index;
  size_t *idom;
  size_t *block_of;
  size_t loop_count;
  size_t *loop_header;
  unsigned char **loop_body;
  size_t *loop_size;
} IRLiveShape;

static uint64_t ir_live_mix(uint64_t hash, uint64_t value) {
  for (int i = 0; i < 8; i++) {
    hash ^= (value >> (i * 8)) & 0xFFu;
    hash *= 1099511628211ull;
  }
  return hash;
}

static uint64_t ir_live_mix_text(uint64_t hash, const char *text) {
  if (!text) {
    return ir_live_mix(hash, 0x9E3779B97F4A7C15ull);
  }
  while (*text) {
    hash ^= (unsigned char)*text++;
    hash *= 1099511628211ull;
  }
  return ir_live_mix(hash, 0xFFu);
}

static int ir_live_map_put(IRLiveMap *map, const char *key, char *value) {
  if ((map->count + 1u) * 2u > map->capacity) {
    size_t grown = map->capacity ? map->capacity * 2u : 64u;
    const char **keys = calloc(grown, sizeof(*keys));
    char **values = calloc(grown, sizeof(*values));
    if (!keys || !values) {
      free(keys);
      free(values);
      return 0;
    }
    for (size_t i = 0; i < map->capacity; i++) {
      if (!map->keys[i]) {
        continue;
      }
      size_t at = (size_t)ir_live_mix_text(1469598103934665603ull,
                                            map->keys[i]) &
                  (grown - 1u);
      while (keys[at]) {
        at = (at + 1u) & (grown - 1u);
      }
      keys[at] = map->keys[i];
      values[at] = map->values[i];
    }
    free(map->keys);
    free(map->values);
    map->keys = keys;
    map->values = values;
    map->capacity = grown;
  }
  size_t at = (size_t)ir_live_mix_text(1469598103934665603ull, key) &
              (map->capacity - 1u);
  while (map->keys[at]) {
    if (strcmp(map->keys[at], key) == 0) {
      free(map->values[at]);
      map->values[at] = value;
      return 1;
    }
    at = (at + 1u) & (map->capacity - 1u);
  }
  map->keys[at] = key;
  map->values[at] = value;
  map->count++;
  return 1;
}

static const char *ir_live_map_get(const IRLiveMap *map, const char *key) {
  if (!map->capacity || !key) {
    return NULL;
  }
  size_t at = (size_t)ir_live_mix_text(1469598103934665603ull, key) &
              (map->capacity - 1u);
  while (map->keys[at]) {
    if (strcmp(map->keys[at], key) == 0) {
      return map->values[at];
    }
    at = (at + 1u) & (map->capacity - 1u);
  }
  return NULL;
}

static void ir_live_map_destroy(IRLiveMap *map) {
  for (size_t i = 0; i < map->capacity; i++) {
    free(map->values[i]);
  }
  free(map->keys);
  free(map->values);
  memset(map, 0, sizeof(*map));
}

static int ir_live_function_movable(const IRFunction *function) {
  return function && !function->is_kernel && !function->is_naked &&
         !function->is_interrupt && !function->is_rule &&
         !function->is_inline_contract && function->name &&
         !strstr(function->name, IR_LIVE_RESUME_INFIX);
}

static void ir_live_declare_helper(IRLoweringContext *context,
                                   const char *name, size_t string_params,
                                   size_t int_params) {
  IRModuleSymbol entry = {0};
  MtlcType *params[6];
  size_t count = 0;
  if (!context || !context->program || !context->type_checker ||
      ir_program_lookup_symbol(context->program, name)) {
    return;
  }
  MtlcType *word = mtlc_type_from_frontend(context->type_checker->builtin_int64);
  MtlcType *text =
      mtlc_type_from_frontend(context->type_checker->builtin_string);
  for (size_t i = 0; i < string_params && count < 6u; i++) {
    params[count++] = text;
  }
  for (size_t i = 0; i < int_params && count < 6u; i++) {
    params[count++] = word;
  }
  entry.name = (char *)name;
  entry.kind = IR_MODSYM_FUNCTION;
  entry.is_extern = 1;
  entry.type = word;
  entry.return_type = word;
  entry.param_types = count ? params : NULL;
  entry.param_count = count;
  ir_program_add_symbol(context->program, &entry);
}

static void ir_live_declare_helpers(IRLoweringContext *context) {
  ir_live_declare_helper(context, IR_LIVE_QUIESCE_NAME, 0, 0);
  ir_live_declare_helper(context, IR_LIVE_PUT_NAME, 1, 3);
  ir_live_declare_helper(context, IR_LIVE_TAKE_NAME, 1, 2);
  ir_live_declare_helper(context, IR_LIVE_ZERO_NAME, 0, 2);
  ir_live_declare_helper(context, IR_LIVE_ALLOC_NAME, 0, 1);
  ir_live_declare_helper(context, IR_LIVE_END_NAME, 0, 0);
  if (context && context->program && context->type_checker &&
      !ir_program_lookup_symbol(context->program, IR_LIVE_CHECK_NAME)) {
    IRModuleSymbol entry = {0};
    MtlcType *params[2];
    MtlcType *word =
        mtlc_type_from_frontend(context->type_checker->builtin_int64);
    params[0] = word;
    params[1] = mtlc_type_from_frontend(context->type_checker->builtin_string);
    entry.name = IR_LIVE_CHECK_NAME;
    entry.kind = IR_MODSYM_FUNCTION;
    entry.is_extern = 1;
    entry.type = word;
    entry.return_type = word;
    entry.param_types = params;
    entry.param_count = 2;
    ir_program_add_symbol(context->program, &entry);
  }
}

static int ir_live_emit_call(IRLoweringContext *context, IRFunction *function,
                             const char *callee, IROperand *arguments,
                             size_t argument_count, const IROperand *dest,
                             SourceLocation location) {
  IRInstruction call = {0};
  call.op = IR_OP_CALL;
  call.location = location;
  call.text = (char *)callee;
  call.dest = dest ? *dest : ir_operand_none();
  call.arguments = argument_count ? arguments : NULL;
  call.argument_count = argument_count;
  return ir_emit(context, function, &call);
}

static int ir_live_emit_simple(IRLoweringContext *context,
                               IRFunction *function, IROpcode op,
                               IROperand dest, IROperand lhs, IROperand rhs,
                               const char *text, SourceLocation location) {
  IRInstruction instruction = {0};
  int ok;
  instruction.op = op;
  instruction.location = location;
  instruction.dest = dest;
  instruction.lhs = lhs;
  instruction.rhs = rhs;
  instruction.text = (char *)text;
  ok = ir_emit(context, function, &instruction);
  ir_operand_destroy(&dest);
  ir_operand_destroy(&lhs);
  ir_operand_destroy(&rhs);
  return ok;
}

static IROperand ir_live_new_temp(IRLoweringContext *context) {
  char *name = ir_new_temp_name(context);
  IROperand operand = ir_operand_temp(name ? name : ".tlive");
  free(name);
  return operand;
}

int ir_lower_live_site(IRLoweringContext *context, IRFunction *function,
                       SourceLocation location) {
  char flag[512];
  IRModuleSymbol entry = {0};
  IROperand arguments[2];
  IROperand address;
  IROperand moved;
  char *resume = NULL;
  int ok;

  ir_live_declare_helpers(context);
  if (!ir_live_function_movable(function)) {
    IROperand none = ir_operand_none();
    return ir_live_emit_call(context, function, IR_LIVE_QUIESCE_NAME, NULL, 0,
                             &none, location);
  }
  snprintf(flag, sizeof(flag), "%s" IR_LIVE_FRAME_SUFFIX, function->name);
  if (!ir_program_lookup_symbol(context->program, flag)) {
    entry.name = flag;
    entry.kind = IR_MODSYM_VARIABLE;
    entry.type = mtlc_type_from_frontend(context->type_checker->builtin_int64);
    ir_program_add_symbol(context->program, &entry);
  }
  address = ir_live_new_temp(context);
  moved = ir_live_new_temp(context);
  resume = ir_new_label_name(context, "live_resume");
  if (!resume ||
      !ir_live_emit_simple(context, function, IR_OP_ADDRESS_OF,
                           ir_operand_copy(&address), ir_operand_symbol(flag),
                           ir_operand_none(), NULL, location)) {
    free(resume);
    ir_operand_destroy(&address);
    ir_operand_destroy(&moved);
    return 0;
  }
  arguments[0] = address;
  arguments[1] = ir_operand_string(function->name);
  ok = ir_live_emit_call(context, function, IR_LIVE_CHECK_NAME, arguments, 2,
                         &moved, location) &&
       ir_live_emit_simple(context, function, IR_OP_BRANCH_ZERO,
                           ir_operand_none(), ir_operand_copy(&moved),
                           ir_operand_none(), resume, location) &&
       ir_emit_label_instruction(context, function, resume, location);
  ir_operand_destroy(&arguments[0]);
  ir_operand_destroy(&arguments[1]);
  ir_operand_destroy(&moved);
  free(resume);
  return ok;
}

static int ir_live_is_site(const IRInstruction *instruction) {
  return instruction->op == IR_OP_CALL && instruction->text &&
         strcmp(instruction->text, IR_LIVE_CHECK_NAME) == 0;
}

static void ir_live_demote(IRFunction *function) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];
    if (!ir_live_is_site(instruction)) {
      continue;
    }
    for (size_t a = 0; a < instruction->argument_count; a++) {
      ir_operand_destroy(&instruction->arguments[a]);
    }
    free(instruction->arguments);
    instruction->arguments = NULL;
    instruction->argument_count = 0;
    ir_operand_destroy(&instruction->dest);
    instruction->dest = ir_operand_none();
    mettle_free_string(instruction->text);
    instruction->text = mettle_strdup(IR_LIVE_QUIESCE_NAME);
    if (i + 1u < function->instruction_count &&
        function->instructions[i + 1u].op == IR_OP_BRANCH_ZERO) {
      IRInstruction *branch = &function->instructions[i + 1u];
      ir_operand_destroy(&branch->lhs);
      branch->lhs = ir_operand_none();
      mettle_free_string(branch->text);
      branch->text = NULL;
      branch->op = IR_OP_NOP;
    }
  }
  ir_function_clear_cfg(function);
  ir_function_touch(function);
}

static uint64_t ir_live_type_hash(const MtlcType *type, unsigned depth) {
  uint64_t hash = 1469598103934665603ull;
  if (!type) {
    return hash;
  }
  hash = ir_live_mix(hash, (uint64_t)type->kind);
  hash = ir_live_mix(hash, (uint64_t)type->size);
  hash = ir_live_mix_text(hash, type->name);
  if (depth >= 6u) {
    return hash;
  }
  if (type->base_type) {
    hash = ir_live_mix(hash, ir_live_type_hash(type->base_type, depth + 1u));
  }
  hash = ir_live_mix(hash, (uint64_t)type->array_size);
  for (size_t i = 0; i < type->field_count; i++) {
    hash = ir_live_mix_text(hash, type->field_names ? type->field_names[i]
                                                    : NULL);
    hash = ir_live_mix(hash, type->field_offsets ? type->field_offsets[i] : i);
    hash = ir_live_mix(hash,
                       ir_live_type_hash(type->field_types
                                             ? type->field_types[i]
                                             : NULL,
                                         depth + 1u));
  }
  for (size_t i = 0; i < type->tagged_variant_count; i++) {
    hash = ir_live_mix_text(hash, type->tagged_variant_names
                                      ? type->tagged_variant_names[i]
                                      : NULL);
    hash = ir_live_mix(hash, type->tagged_variant_tags
                                 ? (uint64_t)type->tagged_variant_tags[i]
                                 : i);
    hash = ir_live_mix(hash, ir_live_type_hash(type->tagged_variant_payloads
                                                   ? type->tagged_variant_payloads[i]
                                                   : NULL,
                                               depth + 1u));
  }
  return hash;
}

static int ir_live_type_is_scalar(const MtlcType *type) {
  if (!type || type->size == 0 || type->size > 8u) {
    return 0;
  }
  switch (type->kind) {
  case MTLC_TYPE_INT8:
  case MTLC_TYPE_INT16:
  case MTLC_TYPE_INT32:
  case MTLC_TYPE_INT64:
  case MTLC_TYPE_UINT8:
  case MTLC_TYPE_UINT16:
  case MTLC_TYPE_UINT32:
  case MTLC_TYPE_UINT64:
  case MTLC_TYPE_BOOL:
  case MTLC_TYPE_FLOAT32:
  case MTLC_TYPE_FLOAT64:
  case MTLC_TYPE_FUNCTION_POINTER:
  case MTLC_TYPE_POINTER:
  case MTLC_TYPE_ENUM:
    return 1;
  default:
    return 0;
  }
}

static char *ir_live_base_name(const char *name) {
  size_t length = strlen(name);
  const char *mark = strstr(name, "$$");
  if (mark) {
    length = (size_t)(mark - name);
  } else if (strncmp(name, "ir_", 3) == 0 || name[0] == '.') {
    size_t end = length;
    while (end > 1u && isdigit((unsigned char)name[end - 1u])) {
      end--;
    }
    if (end < length && end > 1u && name[end - 1u] == '_') {
      end--;
    }
    length = end;
  }
  char *base = malloc(length + 1u);
  if (base) {
    memcpy(base, name, length);
    base[length] = '\0';
  }
  return base;
}

static void ir_live_locals_destroy(IRLiveLocals *locals) {
  for (size_t i = 0; i < locals->count; i++) {
    free(locals->items[i].key);
    ir_operand_destroy(&locals->items[i].initial);
  }
  free(locals->items);
  memset(locals, 0, sizeof(*locals));
}

static IRLiveLocal *ir_live_locals_find(IRLiveLocals *locals,
                                        const char *name) {
  for (size_t i = 0; i < locals->count; i++) {
    if (strcmp(locals->items[i].name, name) == 0) {
      return &locals->items[i];
    }
  }
  return NULL;
}

static void ir_live_find_initial(const IRFunction *function,
                                 IRLiveLocal *local) {
  size_t limit = local->declared_at + 8u;
  for (size_t i = local->declared_at + 1u;
       i < function->instruction_count && i < limit; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op == IR_OP_LABEL || in->op == IR_OP_JUMP ||
        in->op == IR_OP_BRANCH_ZERO || in->op == IR_OP_BRANCH_EQ ||
        in->op == IR_OP_RETURN || in->op == IR_OP_CALL) {
      return;
    }
    if (in->op == IR_OP_ASSIGN && in->dest.kind == IR_OPERAND_SYMBOL &&
        in->dest.name && strcmp(in->dest.name, local->name) == 0) {
      if (in->lhs.kind == IR_OPERAND_INT || in->lhs.kind == IR_OPERAND_FLOAT ||
          in->lhs.kind == IR_OPERAND_STRING) {
        local->initial = ir_operand_copy(&in->lhs);
      }
      return;
    }
    if (in->op == IR_OP_STORE && i >= 2u && in->rhs.kind == IR_OPERAND_INT &&
        in->rhs.int_value > 8 && in->rhs.int_value == local->size) {
      const IRInstruction *target = &function->instructions[i - 2u];
      const IRInstruction *source = &function->instructions[i - 1u];
      if (target->op == IR_OP_ADDRESS_OF && source->op == IR_OP_ADDRESS_OF &&
          target->lhs.name && strcmp(target->lhs.name, local->name) == 0 &&
          source->lhs.kind == IR_OPERAND_SYMBOL && source->lhs.name &&
          strncmp(source->lhs.name, "ir_agg_const_", 13) == 0) {
        local->initial_global = source->lhs.name;
      }
      return;
    }
  }
}

static int ir_live_add_local(IRLoweringContext *context, IRLiveLocals *locals,
                             const char *name, const char *type_text,
                             MtlcType *type, size_t declared_at) {
  IRLiveLocal *grown;
  IRLiveLocal *local;
  char *base;
  size_t seen = 0;
  (void)context;
  if (!type || type->kind == MTLC_TYPE_VOID || type->size == 0) {
    return 0;
  }
  grown = realloc(locals->items, (locals->count + 1u) * sizeof(IRLiveLocal));
  if (!grown) {
    return 0;
  }
  locals->items = grown;
  local = &locals->items[locals->count];
  memset(local, 0, sizeof(*local));
  base = ir_live_base_name(name);
  if (!base) {
    return 0;
  }
  for (size_t i = 0; i < locals->count; i++) {
    char *other = ir_live_base_name(locals->items[i].name);
    if (other && strcmp(other, base) == 0) {
      seen++;
    }
    free(other);
  }
  if (seen) {
    size_t length = strlen(base) + 24u;
    local->key = malloc(length);
    if (local->key) {
      snprintf(local->key, length, "%s#%zu", base, seen + 1u);
    }
    free(base);
  } else {
    local->key = base;
  }
  if (!local->key) {
    return 0;
  }
  local->name = name;
  local->type_text = type_text;
  local->type = type;
  local->size = (long long)type->size;
  local->layout =
      (long long)(ir_live_type_hash(type, 0) & 0x7FFFFFFFFFFFFFFFull);
  local->scalar = ir_live_type_is_scalar(type);
  local->float_bits = type->kind == MTLC_TYPE_FLOAT32   ? 32
                      : type->kind == MTLC_TYPE_FLOAT64 ? 64
                                                        : 0;
  local->is_unsigned =
      type->kind == MTLC_TYPE_UINT8 || type->kind == MTLC_TYPE_UINT16 ||
      type->kind == MTLC_TYPE_UINT32 || type->kind == MTLC_TYPE_UINT64 ||
      type->kind == MTLC_TYPE_BOOL;
  local->declared_at = declared_at;
  local->initial = ir_operand_none();
  locals->count++;
  return 1;
}

static int ir_live_collect_locals(IRLoweringContext *context,
                                  const IRFunction *function,
                                  IRLiveLocals *locals) {
  for (size_t i = 0; i < function->parameter_count; i++) {
    Type *resolved = function->parameter_types
                         ? ir_resolve_named_type(context,
                                                 function->parameter_types[i])
                         : NULL;
    if (!function->parameter_names[i] ||
        !ir_live_add_local(context, locals, function->parameter_names[i],
                           ir_backend_type_name(function->parameter_types[i]),
                           mtlc_type_from_frontend(resolved), 0)) {
      return 0;
    }
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op == IR_OP_INLINE_ASM ||
        (in->op == IR_OP_NOP && in->text &&
         strncmp(in->text, IR_PARALLEL_MARKER_PREFIX,
                 strlen(IR_PARALLEL_MARKER_PREFIX)) == 0) ||
        in->op == IR_OP_ADDRESS_SPACE_ALLOC) {
      return 0;
    }
    if (in->op != IR_OP_DECLARE_LOCAL || in->dest.kind != IR_OPERAND_SYMBOL ||
        !in->dest.name || ir_live_locals_find(locals, in->dest.name)) {
      continue;
    }
    MtlcType *type = in->value_type;
    if (!type && in->text) {
      type = mtlc_type_from_frontend(ir_resolve_named_type(context, in->text));
    }
    if (!ir_live_add_local(context, locals, in->dest.name, in->text, type, i)) {
      return 0;
    }
    ir_live_find_initial(function, &locals->items[locals->count - 1u]);
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op == IR_OP_ADDRESS_OF && in->lhs.kind == IR_OPERAND_SYMBOL &&
        in->lhs.name) {
      IRLiveLocal *local = ir_live_locals_find(locals, in->lhs.name);
      if (local) {
        local->address_taken = 1;
      }
    }
  }
  return 1;
}

static char *ir_live_local_name(const char *name, const char *suffix) {
  size_t length = strlen(name) + strlen(suffix) + 1u;
  char *text = malloc(length);
  if (text) {
    snprintf(text, length, "%s%s", name, suffix);
  }
  return text;
}

static char *ir_live_resume_name(const char *owner, size_t site) {
  size_t length = strlen(owner) + 32u;
  char *text = malloc(length);
  if (text) {
    snprintf(text, length, "%s" IR_LIVE_RESUME_INFIX "%zu", owner, site);
  }
  return text;
}

static int ir_live_emit_put(IRLoweringContext *context, IRFunction *out,
                            const IRLiveLocal *local, SourceLocation location) {
  IROperand place = ir_live_new_temp(context);
  IROperand arguments[4];
  IROperand none = ir_operand_none();
  int ok = 1;
  if (local->scalar && !local->address_taken) {
    char *save = ir_live_local_name(local->name, ".live");
    IRInstruction declare = {0};
    IRInstruction assign = {0};
    if (!save) {
      ir_operand_destroy(&place);
      return 0;
    }
    declare.op = IR_OP_DECLARE_LOCAL;
    declare.location = location;
    declare.dest = ir_operand_symbol(save);
    declare.text = (char *)local->type_text;
    declare.value_type = local->type;
    assign.op = IR_OP_ASSIGN;
    assign.location = location;
    assign.dest = ir_operand_symbol(save);
    assign.lhs = ir_operand_symbol(local->name);
    if (local->float_bits) {
      ir_assign_apply_float_bits(&assign, &assign.lhs, local->float_bits);
    }
    ok = ir_emit(context, out, &declare) && ir_emit(context, out, &assign) &&
         ir_live_emit_simple(context, out, IR_OP_ADDRESS_OF,
                             ir_operand_copy(&place), ir_operand_symbol(save),
                             ir_operand_none(), NULL, location);
    ir_operand_destroy(&declare.dest);
    ir_operand_destroy(&assign.dest);
    ir_operand_destroy(&assign.lhs);
    free(save);
  } else {
    ok = ir_live_emit_simple(context, out, IR_OP_ADDRESS_OF,
                             ir_operand_copy(&place),
                             ir_operand_symbol(local->name), ir_operand_none(),
                             NULL, location);
  }
  arguments[0] = ir_operand_string(local->key);
  arguments[1] = ir_operand_int(local->layout);
  arguments[2] = ir_operand_int(local->size);
  arguments[3] = place;
  ok = ok && ir_live_emit_call(context, out, IR_LIVE_PUT_NAME, arguments, 4,
                               &none, location);
  for (size_t i = 0; i < 4; i++) {
    ir_operand_destroy(&arguments[i]);
  }
  return ok;
}

static int ir_live_emit_transfer(IRLoweringContext *context, IRFunction *out,
                                 const IRFunction *function,
                                 const IRLiveLocals *locals,
                                 const IRLiveSite *site, size_t site_number) {
  SourceLocation location = function->location;
  char *resume = ir_live_resume_name(function->name, site_number);
  IROperand result = ir_operand_none();
  IRInstruction call = {0};
  IRInstruction back = {0};
  Type *return_type = NULL;
  int ok = resume != NULL;
  for (size_t i = 0; ok && i < site->local_count; i++) {
    ok = ir_live_emit_put(context, out, &locals->items[i], location);
  }
  if (function->return_type_name &&
      strcmp(function->return_type_name, "void") != 0) {
    return_type = ir_resolve_named_type(context, function->return_type_name);
    result = ir_live_new_temp(context);
  }
  call.op = IR_OP_CALL;
  call.location = location;
  call.text = resume;
  call.dest = result;
  call.value_type = return_type ? mtlc_type_from_frontend(return_type) : NULL;
  ok = ok && ir_emit(context, out, &call);
  back.op = IR_OP_RETURN;
  back.location = location;
  back.lhs = result;
  if (result.kind != IR_OPERAND_NONE) {
    int bits = ir_named_type_float_bits(context, function->return_type_name);
    if (bits) {
      ir_assign_apply_float_bits(&back, &back.lhs, bits);
    }
  }
  ok = ok && ir_emit(context, out, &back);
  ir_operand_destroy(&result);
  free(resume);
  return ok;
}

static void ir_live_shape_destroy(IRLiveShape *shape) {
  free(shape->rpo);
  free(shape->rpo_index);
  free(shape->idom);
  free(shape->block_of);
  for (size_t i = 0; i < shape->loop_count; i++) {
    free(shape->loop_body[i]);
  }
  free(shape->loop_header);
  free(shape->loop_body);
  free(shape->loop_size);
  memset(shape, 0, sizeof(*shape));
}

static int ir_live_dominates(const IRLiveShape *shape, size_t a, size_t b) {
  if (shape->rpo_index[b] == SIZE_MAX || shape->rpo_index[a] == SIZE_MAX) {
    return 0;
  }
  for (;;) {
    if (a == b) {
      return 1;
    }
    if (shape->idom[b] == b) {
      return 0;
    }
    b = shape->idom[b];
  }
}

static int ir_live_shape_build(IRFunction *function, IRLiveShape *shape) {
  size_t n = function->block_count;
  size_t *stack = NULL;
  size_t *next = NULL;
  unsigned char *seen = NULL;
  size_t depth = 0;
  size_t order = 0;
  int ok = 0;
  memset(shape, 0, sizeof(*shape));
  shape->block_count = n;
  if (n == 0) {
    return 0;
  }
  shape->rpo = calloc(n, sizeof(size_t));
  shape->rpo_index = malloc(n * sizeof(size_t));
  shape->idom = malloc(n * sizeof(size_t));
  shape->block_of = calloc(function->instruction_count + 1u, sizeof(size_t));
  stack = malloc(n * sizeof(size_t));
  next = calloc(n, sizeof(size_t));
  seen = calloc(n, 1u);
  if (!shape->rpo || !shape->rpo_index || !shape->idom || !shape->block_of ||
      !stack || !next || !seen) {
    goto done;
  }
  for (size_t b = 0; b < n; b++) {
    const IRBasicBlock *block = &function->blocks[b];
    shape->rpo_index[b] = SIZE_MAX;
    shape->idom[b] = SIZE_MAX;
    for (size_t i = 0; i < block->instruction_count; i++) {
      shape->block_of[block->first_instruction + i] = b;
    }
  }
  {
    size_t *post = malloc(n * sizeof(size_t));
    if (!post) {
      goto done;
    }
    stack[depth++] = function->entry_block;
    seen[function->entry_block] = 1;
    while (depth) {
      size_t top = stack[depth - 1u];
      const IRBasicBlock *block = &function->blocks[top];
      if (next[top] < block->successor_count) {
        size_t successor = block->successors[next[top]++];
        if (!seen[successor]) {
          seen[successor] = 1;
          stack[depth++] = successor;
        }
        continue;
      }
      post[order++] = top;
      depth--;
    }
    for (size_t i = 0; i < order; i++) {
      shape->rpo[i] = post[order - 1u - i];
      shape->rpo_index[shape->rpo[i]] = i;
    }
    free(post);
  }
  shape->idom[function->entry_block] = function->entry_block;
  for (int changed = 1; changed;) {
    changed = 0;
    for (size_t i = 1; i < order; i++) {
      size_t b = shape->rpo[i];
      const IRBasicBlock *block = &function->blocks[b];
      size_t chosen = SIZE_MAX;
      for (size_t p = 0; p < block->predecessor_count; p++) {
        size_t pred = block->predecessors[p];
        if (shape->idom[pred] == SIZE_MAX) {
          continue;
        }
        if (chosen == SIZE_MAX) {
          chosen = pred;
          continue;
        }
        size_t x = pred;
        size_t y = chosen;
        while (x != y) {
          while (shape->rpo_index[x] > shape->rpo_index[y]) {
            x = shape->idom[x];
          }
          while (shape->rpo_index[y] > shape->rpo_index[x]) {
            y = shape->idom[y];
          }
        }
        chosen = x;
      }
      if (chosen != SIZE_MAX && shape->idom[b] != chosen) {
        shape->idom[b] = chosen;
        changed = 1;
      }
    }
  }
  for (size_t i = 0; i < order; i++) {
    size_t u = shape->rpo[i];
    const IRBasicBlock *block = &function->blocks[u];
    for (size_t s = 0; s < block->successor_count; s++) {
      size_t h = block->successors[s];
      size_t loop = SIZE_MAX;
      if (!ir_live_dominates(shape, h, u)) {
        continue;
      }
      for (size_t l = 0; l < shape->loop_count; l++) {
        if (shape->loop_header[l] == h) {
          loop = l;
        }
      }
      if (loop == SIZE_MAX) {
        size_t *headers = realloc(shape->loop_header,
                                  (shape->loop_count + 1u) * sizeof(size_t));
        unsigned char **bodies = NULL;
        size_t *sizes = NULL;
        if (headers) {
          shape->loop_header = headers;
        }
        bodies = realloc(shape->loop_body, (shape->loop_count + 1u) *
                                               sizeof(unsigned char *));
        if (bodies) {
          shape->loop_body = bodies;
        }
        sizes = realloc(shape->loop_size,
                        (shape->loop_count + 1u) * sizeof(size_t));
        if (sizes) {
          shape->loop_size = sizes;
        }
        if (!headers || !bodies || !sizes) {
          goto done;
        }
        loop = shape->loop_count;
        shape->loop_header[loop] = h;
        shape->loop_body[loop] = calloc(n, 1u);
        shape->loop_size[loop] = 1;
        shape->loop_count++;
        if (!shape->loop_body[loop]) {
          goto done;
        }
        shape->loop_body[loop][h] = 1;
      }
      depth = 0;
      if (!shape->loop_body[loop][u]) {
        shape->loop_body[loop][u] = 1;
        shape->loop_size[loop]++;
        stack[depth++] = u;
      }
      while (depth) {
        size_t x = stack[--depth];
        const IRBasicBlock *xb = &function->blocks[x];
        for (size_t p = 0; p < xb->predecessor_count; p++) {
          size_t pred = xb->predecessors[p];
          if (shape->rpo_index[pred] == SIZE_MAX ||
              shape->loop_body[loop][pred]) {
            continue;
          }
          shape->loop_body[loop][pred] = 1;
          shape->loop_size[loop]++;
          stack[depth++] = pred;
        }
      }
    }
  }
  ok = 1;
done:
  free(stack);
  free(next);
  free(seen);
  return ok;
}

static size_t ir_live_label_block(const IRFunction *function,
                                  const char *label) {
  if (!label) {
    return SIZE_MAX;
  }
  for (size_t b = 0; b < function->block_count; b++) {
    if (function->blocks[b].label &&
        strcmp(function->blocks[b].label, label) == 0) {
      return b;
    }
  }
  return SIZE_MAX;
}

static char *ir_live_fresh_label(IRLoweringContext *context,
                                 const char *original) {
  char prefix[96];
  size_t length = original ? strlen(original) : 0;
  if (original && strncmp(original, "ir_", 3) == 0) {
    size_t end = length;
    while (end > 3u && isdigit((unsigned char)original[end - 1u])) {
      end--;
    }
    if (end < length && end > 4u && original[end - 1u] == '_') {
      end--;
      if (end - 3u < sizeof(prefix)) {
        memcpy(prefix, original + 3, end - 3u);
        prefix[end - 3u] = '\0';
        return ir_new_label_name(context, prefix);
      }
    }
  }
  return ir_new_label_name(context, "live");
}

static const char *ir_live_rename(IRLoweringContext *context, IRLiveMap *map,
                                  const char *name, int is_label) {
  const char *have = ir_live_map_get(map, name);
  char *fresh;
  if (have) {
    return have;
  }
  if (is_label) {
    fresh = ir_live_fresh_label(context, name);
  } else {
    fresh = ir_new_temp_name(context);
  }
  if (!fresh || !ir_live_map_put(map, name, fresh)) {
    free(fresh);
    return NULL;
  }
  return fresh;
}

static int ir_live_rename_operand(IRLoweringContext *context, IRLiveMap *temps,
                                  IROperand *operand) {
  operand->value_id = 0;
  if (operand->kind != IR_OPERAND_TEMP || !operand->name) {
    return 1;
  }
  const char *renamed = ir_live_rename(context, temps, operand->name, 0);
  if (!renamed) {
    return 0;
  }
  operand->name = (char *)renamed;
  return 1;
}

static int ir_live_is_jump(const IRInstruction *instruction) {
  return instruction->op == IR_OP_JUMP ||
         instruction->op == IR_OP_BRANCH_ZERO ||
         instruction->op == IR_OP_BRANCH_EQ;
}

static int ir_live_copy_instruction(IRLoweringContext *context,
                                    IRFunction *out,
                                    const IRInstruction *source,
                                    IRLiveMap *labels, IRLiveMap *temps,
                                    const char *target_override) {
  IRInstruction copy = *source;
  IROperand *arguments = NULL;
  int ok;
  if (!ir_live_rename_operand(context, temps, &copy.dest) ||
      !ir_live_rename_operand(context, temps, &copy.lhs) ||
      !ir_live_rename_operand(context, temps, &copy.rhs)) {
    return 0;
  }
  if (source->argument_count) {
    arguments = malloc(source->argument_count * sizeof(IROperand));
    if (!arguments) {
      return 0;
    }
    for (size_t a = 0; a < source->argument_count; a++) {
      arguments[a] = source->arguments[a];
      if (!ir_live_rename_operand(context, temps, &arguments[a])) {
        free(arguments);
        return 0;
      }
    }
    copy.arguments = arguments;
  }
  if (source->op == IR_OP_LABEL || ir_live_is_jump(source)) {
    if (target_override) {
      copy.text = (char *)target_override;
    } else {
      copy.text = (char *)ir_live_rename(context, labels, source->text, 1);
      if (!copy.text) {
        free(arguments);
        return 0;
      }
    }
  }
  ok = ir_function_append_instruction(out, &copy);
  free(arguments);
  return ok;
}

typedef struct {
  size_t header;
  size_t loop;
  char *land;
} IRLiveLevel;

static int ir_live_level_compare(const void *a, const void *b) {
  const IRLiveLevel *x = (const IRLiveLevel *)a;
  const IRLiveLevel *y = (const IRLiveLevel *)b;
  return x->loop < y->loop ? 1 : (x->loop > y->loop ? -1 : 0);
}

static int ir_live_emit_load_into(IRLoweringContext *context, IRFunction *out,
                                  const IRLiveLocal *local,
                                  const IROperand *source,
                                  SourceLocation location) {
  IRInstruction load = {0};
  IRInstruction assign = {0};
  IROperand value = ir_live_new_temp(context);
  int ok;
  load.op = IR_OP_LOAD;
  load.location = location;
  load.dest = value;
  load.lhs = *source;
  load.rhs = ir_operand_int(local->size);
  load.value_type = local->type;
  load.is_unsigned = local->is_unsigned;
  if (local->float_bits) {
    load.is_float = 1;
    load.float_bits = local->float_bits;
    load.dest.float_bits = local->float_bits;
    value.float_bits = local->float_bits;
  }
  assign.op = IR_OP_ASSIGN;
  assign.location = location;
  assign.dest = ir_operand_symbol(local->name);
  assign.lhs = value;
  if (local->float_bits) {
    ir_assign_apply_float_bits(&assign, &assign.lhs, local->float_bits);
  }
  ok = ir_emit(context, out, &load) && ir_emit(context, out, &assign);
  ir_operand_destroy(&assign.dest);
  ir_operand_destroy(&value);
  return ok;
}

static int ir_live_emit_default_scalar(IRLoweringContext *context,
                                       IRFunction *out,
                                       const IRLiveLocal *local,
                                       SourceLocation location) {
  IRInstruction assign = {0};
  int ok;
  assign.op = IR_OP_ASSIGN;
  assign.location = location;
  assign.dest = ir_operand_symbol(local->name);
  if (local->initial.kind != IR_OPERAND_NONE) {
    assign.lhs = ir_operand_copy(&local->initial);
  } else if (local->float_bits) {
    assign.lhs = ir_operand_float_sized(0.0, local->float_bits);
  } else {
    assign.lhs = ir_operand_int(0);
  }
  if (local->float_bits) {
    ir_assign_apply_float_bits(&assign, &assign.lhs, local->float_bits);
  }
  ok = ir_emit(context, out, &assign);
  ir_operand_destroy(&assign.dest);
  ir_operand_destroy(&assign.lhs);
  return ok;
}

static int ir_live_emit_copy_bytes(IRLoweringContext *context,
                                   IRFunction *out, const IRLiveLocal *local,
                                   const IROperand *target,
                                   const IROperand *source,
                                   SourceLocation location) {
  IRInstruction store = {0};
  IROperand value = ir_operand_none();
  int ok = 1;
  store.op = IR_OP_STORE;
  store.location = location;
  store.dest = *target;
  store.rhs = ir_operand_int(local->size);
  if (local->size > 8) {
    store.lhs = *source;
  } else {
    IRInstruction load = {0};
    value = ir_live_new_temp(context);
    load.op = IR_OP_LOAD;
    load.location = location;
    load.dest = value;
    load.lhs = *source;
    load.rhs = ir_operand_int(local->size);
    ok = ir_emit(context, out, &load);
    store.lhs = value;
  }
  ok = ok && ir_emit(context, out, &store);
  ir_operand_destroy(&value);
  return ok;
}

static int ir_live_emit_default_memory(IRLoweringContext *context,
                                       IRFunction *out,
                                       const IRLiveLocal *local,
                                       const IROperand *place,
                                       SourceLocation location) {
  if (local->initial.kind != IR_OPERAND_NONE) {
    IRInstruction assign = {0};
    int ok;
    assign.op = IR_OP_ASSIGN;
    assign.location = location;
    assign.dest = ir_operand_symbol(local->name);
    assign.lhs = ir_operand_copy(&local->initial);
    ok = ir_emit(context, out, &assign);
    ir_operand_destroy(&assign.dest);
    ir_operand_destroy(&assign.lhs);
    return ok;
  }
  if (local->initial_global) {
    IROperand source = ir_live_new_temp(context);
    int ok = ir_live_emit_simple(context, out, IR_OP_ADDRESS_OF,
                                 ir_operand_copy(&source),
                                 ir_operand_symbol(local->initial_global),
                                 ir_operand_none(), NULL, location) &&
             ir_live_emit_copy_bytes(context, out, local, place, &source,
                                     location);
    ir_operand_destroy(&source);
    return ok;
  }
  IROperand arguments[2];
  IROperand none = ir_operand_none();
  int ok;
  arguments[0] = ir_operand_copy(place);
  arguments[1] = ir_operand_int(local->size);
  ok = ir_live_emit_call(context, out, IR_LIVE_ZERO_NAME, arguments, 2, &none,
                         location);
  ir_operand_destroy(&arguments[0]);
  ir_operand_destroy(&arguments[1]);
  return ok;
}

static int ir_live_emit_take(IRLoweringContext *context, IRFunction *out,
                             const IRLiveLocal *local, IROperand *found,
                             SourceLocation location) {
  IROperand arguments[3];
  int ok;
  *found = ir_live_new_temp(context);
  arguments[0] = ir_operand_string(local->key);
  arguments[1] = ir_operand_int(local->layout);
  arguments[2] = ir_operand_int(local->size);
  ok = ir_live_emit_call(context, out, IR_LIVE_TAKE_NAME, arguments, 3, found,
                         location);
  for (size_t i = 0; i < 3; i++) {
    ir_operand_destroy(&arguments[i]);
  }
  return ok;
}

static int ir_live_emit_restore(IRLoweringContext *context, IRFunction *out,
                                const IRLiveLocal *local, int address_only,
                                SourceLocation location) {
  IRInstruction declare = {0};
  IROperand found = ir_operand_none();
  IROperand place = ir_operand_none();
  char *fallback = ir_new_label_name(context, "live_default");
  char *done = ir_new_label_name(context, "live_restored");
  char *at = address_only ? ir_live_local_name(local->name, ".at") : NULL;
  int ok = fallback && done && (!address_only || at);
  declare.op = IR_OP_DECLARE_LOCAL;
  declare.location = location;
  declare.dest = ir_operand_symbol(local->name);
  declare.text = (char *)local->type_text;
  declare.value_type = local->type;
  ok = ok && (address_only || ir_emit(context, out, &declare));
  ir_operand_destroy(&declare.dest);
  if (ok && address_only) {
    IRInstruction pointer = {0};
    pointer.op = IR_OP_DECLARE_LOCAL;
    pointer.location = location;
    pointer.dest = ir_operand_symbol(at);
    pointer.text = "rawptr";
    pointer.value_type =
        mtlc_type_from_frontend(context->type_checker->builtin_rawptr);
    ok = ir_emit(context, out, &pointer);
    ir_operand_destroy(&pointer.dest);
  }
  ok = ok && ir_live_emit_take(context, out, local, &found, location) &&
       ir_live_emit_simple(context, out, IR_OP_BRANCH_ZERO, ir_operand_none(),
                           ir_operand_copy(&found), ir_operand_none(),
                           fallback, location);
  if (ok && address_only) {
    IROperand size = ir_operand_int(local->size);
    place = ir_live_new_temp(context);
    ok = ir_live_emit_simple(context, out, IR_OP_ASSIGN,
                             ir_operand_symbol(at), ir_operand_copy(&found),
                             ir_operand_none(), NULL, location) &&
         ir_emit_jump_instruction(context, out, done, location) &&
         ir_emit_label_instruction(context, out, fallback, location) &&
         ir_live_emit_call(context, out, IR_LIVE_ALLOC_NAME, &size, 1, &place,
                           location) &&
         ir_live_emit_simple(context, out, IR_OP_ASSIGN,
                             ir_operand_symbol(at), ir_operand_copy(&place),
                             ir_operand_none(), NULL, location);
    if (ok && local->initial_global) {
      IROperand source = ir_live_new_temp(context);
      ok = ir_live_emit_simple(context, out, IR_OP_ADDRESS_OF,
                               ir_operand_copy(&source),
                               ir_operand_symbol(local->initial_global),
                               ir_operand_none(), NULL, location) &&
           ir_live_emit_copy_bytes(context, out, local, &place, &source,
                                   location);
      ir_operand_destroy(&source);
    }
    ir_operand_destroy(&size);
  } else if (ok && local->scalar) {
    ok = ir_live_emit_load_into(context, out, local, &found, location) &&
         ir_emit_jump_instruction(context, out, done, location) &&
         ir_emit_label_instruction(context, out, fallback, location) &&
         ir_live_emit_default_scalar(context, out, local, location);
  } else if (ok) {
    place = ir_live_new_temp(context);
    ok = ir_live_emit_simple(context, out, IR_OP_ADDRESS_OF,
                             ir_operand_copy(&place),
                             ir_operand_symbol(local->name), ir_operand_none(),
                             NULL, location) &&
         ir_live_emit_copy_bytes(context, out, local, &place, &found,
                                 location) &&
         ir_emit_jump_instruction(context, out, done, location) &&
         ir_emit_label_instruction(context, out, fallback, location) &&
         ir_live_emit_simple(context, out, IR_OP_ADDRESS_OF,
                             ir_operand_copy(&place),
                             ir_operand_symbol(local->name), ir_operand_none(),
                             NULL, location) &&
         ir_live_emit_default_memory(context, out, local, &place, location);
  }
  ok = ok && ir_emit_label_instruction(context, out, done, location);
  ir_operand_destroy(&found);
  ir_operand_destroy(&place);
  free(fallback);
  free(done);
  free(at);
  return ok;
}

static int ir_live_symbol_mentioned(const IRInstruction *in,
                                    const char *name) {
  if ((in->dest.kind == IR_OPERAND_SYMBOL && in->dest.name &&
       strcmp(in->dest.name, name) == 0) ||
      (in->lhs.kind == IR_OPERAND_SYMBOL && in->lhs.name &&
       strcmp(in->lhs.name, name) == 0) ||
      (in->rhs.kind == IR_OPERAND_SYMBOL && in->rhs.name &&
       strcmp(in->rhs.name, name) == 0)) {
    return 1;
  }
  for (size_t a = 0; a < in->argument_count; a++) {
    if (in->arguments[a].kind == IR_OPERAND_SYMBOL &&
        in->arguments[a].name && strcmp(in->arguments[a].name, name) == 0) {
      return 1;
    }
  }
  return 0;
}

static int ir_live_address_only(const IRFunction *body, size_t start,
                                const char *name) {
  for (size_t i = start; i < body->instruction_count; i++) {
    const IRInstruction *in = &body->instructions[i];
    if (!ir_live_symbol_mentioned(in, name)) {
      continue;
    }
    if (in->op == IR_OP_DECLARE_LOCAL) {
      continue;
    }
    if (in->op == IR_OP_ADDRESS_OF && in->lhs.kind == IR_OPERAND_SYMBOL &&
        in->dest.kind != IR_OPERAND_SYMBOL && in->rhs.kind == IR_OPERAND_NONE &&
        in->argument_count == 0) {
      continue;
    }
    return 0;
  }
  return 1;
}

static IRFunction *ir_live_build_continuation(IRLoweringContext *context,
                                              IRFunction *function,
                                              const IRLiveShape *shape,
                                              const IRLiveLocals *locals,
                                              const IRLiveSite *site,
                                              size_t site_number) {
  IRLiveLevel *levels = NULL;
  size_t level_count = 0;
  IRLiveMap *labels = NULL;
  IRLiveMap *temps = NULL;
  IRFunction *staging = NULL;
  IRFunction *clone = NULL;
  unsigned char *reachable = NULL;
  unsigned char *address_only = NULL;
  char *name = ir_live_resume_name(function->name, site_number);
  char *start = ir_new_label_name(context, "live_start");
  size_t resume_block = ir_live_label_block(function, site->resume_label);
  const char *entry_target = NULL;
  int ok = 0;

  if (!name || !start || resume_block == SIZE_MAX ||
      shape->rpo_index[resume_block] == SIZE_MAX) {
    goto done;
  }
  for (size_t l = 0; l < shape->loop_count; l++) {
    if (!shape->loop_body[l][resume_block]) {
      continue;
    }
    IRLiveLevel *grown =
        realloc(levels, (level_count + 1u) * sizeof(IRLiveLevel));
    if (!grown) {
      goto done;
    }
    levels = grown;
    levels[level_count].header = shape->loop_header[l];
    levels[level_count].loop = shape->loop_size[l];
    levels[level_count].land = NULL;
    level_count++;
  }
  qsort(levels, level_count, sizeof(IRLiveLevel), ir_live_level_compare);
  for (size_t j = 0; j < level_count; j++) {
    size_t loop = SIZE_MAX;
    const IRBasicBlock *header = &function->blocks[levels[j].header];
    for (size_t l = 0; l < shape->loop_count; l++) {
      if (shape->loop_header[l] == levels[j].header) {
        loop = l;
      }
    }
    levels[j].loop = loop;
    if (loop == SIZE_MAX || !header->label || header->instruction_count == 0 ||
        header->instructions[0].op != IR_OP_LABEL) {
      goto done;
    }
    for (size_t b = 0; b < function->block_count; b++) {
      const IRBasicBlock *block = &function->blocks[b];
      const IRInstruction *last;
      if (!shape->loop_body[loop][b] || block->instruction_count == 0 ||
          b + 1u != levels[j].header) {
        continue;
      }
      last = &block->instructions[block->instruction_count - 1u];
      if (last->op != IR_OP_JUMP && last->op != IR_OP_RETURN) {
        goto done;
      }
    }
    levels[j].land = ir_new_label_name(context, "live_land");
    if (!levels[j].land) {
      goto done;
    }
  }
  labels = calloc(level_count + 1u, sizeof(IRLiveMap));
  temps = calloc(level_count + 1u, sizeof(IRLiveMap));
  staging = ir_function_create(name);
  if (!labels || !temps || !staging) {
    goto done;
  }
  if (!ir_emit_label_instruction(context, staging, start, function->location)) {
    goto done;
  }
  entry_target = ir_live_rename(context, &labels[level_count],
                                site->resume_label, 1);
  if (!entry_target ||
      !ir_emit_jump_instruction(context, staging, entry_target,
                                function->location)) {
    goto done;
  }
  for (size_t c = level_count + 1u; c-- > 0;) {
    for (size_t i = 0; i < function->instruction_count; i++) {
      const IRInstruction *in = &function->instructions[i];
      size_t block = shape->block_of[i];
      const char *target = NULL;
      if (in->op == IR_OP_NOP && in->text && strncmp(in->text, "@@", 2) == 0) {
        continue;
      }
      if (c < level_count &&
          i == function->blocks[levels[c].header].first_instruction &&
          !ir_emit_label_instruction(context, staging, levels[c].land,
                                     function->location)) {
        goto done;
      }
      if (ir_live_is_jump(in)) {
        size_t to = ir_live_label_block(function, in->text);
        for (size_t j = 0; j < c && j < level_count; j++) {
          if (to == levels[j].header && shape->loop_body[levels[j].loop][block]) {
            target = levels[j].land;
            break;
          }
        }
      }
      if (!ir_live_copy_instruction(context, staging, in, &labels[c],
                                    &temps[c], target)) {
        goto done;
      }
    }
  }
  if (!ir_function_rebuild_cfg(staging)) {
    goto done;
  }
  reachable = calloc(staging->block_count ? staging->block_count : 1u, 1u);
  if (!reachable) {
    goto done;
  }
  {
    size_t *stack = malloc((staging->block_count + 1u) * sizeof(size_t));
    size_t depth = 0;
    if (!stack) {
      goto done;
    }
    reachable[staging->entry_block] = 1;
    stack[depth++] = staging->entry_block;
    while (depth) {
      const IRBasicBlock *block = &staging->blocks[stack[--depth]];
      for (size_t s = 0; s < block->successor_count; s++) {
        if (!reachable[block->successors[s]]) {
          reachable[block->successors[s]] = 1;
          stack[depth++] = block->successors[s];
        }
      }
    }
    free(stack);
  }
  clone = ir_function_create(name);
  if (!clone) {
    goto done;
  }
  clone->location = function->location;
  clone->is_noinline = 1;
  clone->is_exported = 0;
  if (function->return_type_name) {
    clone->return_type_name = mettle_strdup(function->return_type_name);
    if (!clone->return_type_name) {
      goto done;
    }
  }
  {
    IRFunction *kept = ir_function_create(name);
    if (!kept) {
      goto done;
    }
    for (size_t b = 0; b < staging->block_count; b++) {
      const IRBasicBlock *block = &staging->blocks[b];
      if (b == staging->entry_block || !reachable[b]) {
        continue;
      }
      for (size_t i = 0; i < block->instruction_count; i++) {
        if (!ir_function_append_instruction(kept, &block->instructions[i])) {
          ir_function_destroy(kept);
          goto done;
        }
      }
    }
    ir_function_destroy(staging);
    staging = kept;
  }
  address_only = calloc(site->local_count ? site->local_count : 1u, 1u);
  if (!address_only) {
    goto done;
  }
  {
    char *entry = ir_new_label_name(context, "entry");
    int emitted = entry && ir_emit_label_instruction(context, clone, entry,
                                                     function->location);
    free(entry);
    if (!emitted) {
      goto done;
    }
  }
  for (size_t i = 0; i < site->local_count; i++) {
    const IRLiveLocal *local = &locals->items[i];
    address_only[i] = (unsigned char)ir_live_address_only(staging, 0,
                                                          local->name);
    if (!ir_live_emit_restore(context, clone, local, address_only[i],
                              function->location)) {
      goto done;
    }
  }
  {
    IROperand none = ir_operand_none();
    if (!ir_live_emit_call(context, clone, IR_LIVE_END_NAME, NULL, 0, &none,
                           function->location) ||
        !ir_emit_jump_instruction(context, clone, entry_target,
                                  function->location)) {
      goto done;
    }
  }
  for (size_t i = 0; i < staging->instruction_count; i++) {
    const IRInstruction *in = &staging->instructions[i];
    const IRLiveLocal *rewrite = NULL;
    if (in->op == IR_OP_ADDRESS_OF && in->lhs.kind == IR_OPERAND_SYMBOL &&
        in->lhs.name) {
      for (size_t l = 0; l < site->local_count; l++) {
        if (address_only[l] &&
            strcmp(locals->items[l].name, in->lhs.name) == 0) {
          rewrite = &locals->items[l];
          break;
        }
      }
    }
    if (in->op == IR_OP_DECLARE_LOCAL && in->dest.kind == IR_OPERAND_SYMBOL &&
        in->dest.name) {
      int dropped = 0;
      for (size_t l = 0; l < site->local_count && !dropped; l++) {
        dropped = address_only[l] &&
                  strcmp(locals->items[l].name, in->dest.name) == 0;
      }
      if (dropped) {
        continue;
      }
    }
    if (rewrite) {
      char *at = ir_live_local_name(rewrite->name, ".at");
      IRInstruction assign = {0};
      int appended;
      if (!at) {
        goto done;
      }
      assign.op = IR_OP_ASSIGN;
      assign.location = in->location;
      assign.dest = in->dest;
      assign.lhs = ir_operand_symbol(at);
      appended = ir_function_append_instruction(clone, &assign);
      ir_operand_destroy(&assign.lhs);
      free(at);
      if (!appended) {
        goto done;
      }
      continue;
    }
    if (!ir_function_append_instruction(clone, in)) {
      goto done;
    }
  }
  if (!ir_function_rebuild_cfg(clone)) {
    goto done;
  }
  ok = 1;

done:
  if (levels) {
    for (size_t j = 0; j < level_count; j++) {
      free(levels[j].land);
    }
    free(levels);
  }
  if (labels) {
    for (size_t c = 0; c <= level_count; c++) {
      ir_live_map_destroy(&labels[c]);
    }
    free(labels);
  }
  if (temps) {
    for (size_t c = 0; c <= level_count; c++) {
      ir_live_map_destroy(&temps[c]);
    }
    free(temps);
  }
  if (staging) {
    ir_function_destroy(staging);
  }
  free(reachable);
  free(address_only);
  free(name);
  free(start);
  if (!ok && clone) {
    ir_function_destroy(clone);
    clone = NULL;
  }
  return clone;
}

int ir_lower_live_frames(IRLoweringContext *context, IRFunction *function) {
  IRLiveLocals locals = {0};
  IRLiveSite *sites = NULL;
  size_t site_count = 0;
  IRFunction *rebuilt = NULL;
  IRFunction **clones = NULL;
  IRLiveShape shape;
  int usable = 1;
  int ok = 1;

  memset(&shape, 0, sizeof(shape));
  if (!context || !function || !g_ir_lowering_live) {
    return 1;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (!ir_live_is_site(&function->instructions[i])) {
      continue;
    }
    IRLiveSite *grown = realloc(sites, (site_count + 1u) * sizeof(IRLiveSite));
    if (!grown) {
      free(sites);
      return 0;
    }
    sites = grown;
    sites[site_count].call_index = i;
    sites[site_count].resume_label = NULL;
    sites[site_count].local_count = 0;
    if (i + 2u < function->instruction_count &&
        function->instructions[i + 1u].op == IR_OP_BRANCH_ZERO &&
        function->instructions[i + 2u].op == IR_OP_LABEL) {
      sites[site_count].resume_label = function->instructions[i + 2u].text;
    } else {
      usable = 0;
    }
    site_count++;
  }
  if (site_count == 0) {
    return 1;
  }
  function->is_noinline = 1;
  if (!usable || !ir_live_collect_locals(context, function, &locals)) {
    goto demote;
  }
  for (size_t s = 0; s < site_count; s++) {
    while (sites[s].local_count < locals.count &&
           locals.items[sites[s].local_count].declared_at <
               sites[s].call_index) {
      sites[s].local_count++;
    }
  }
  rebuilt = ir_function_create(function->name);
  if (!rebuilt) {
    ok = 0;
    goto cleanup;
  }
  {
    size_t next_site = 0;
    for (size_t i = 0; i < function->instruction_count; i++) {
      if (!ir_function_append_instruction(rebuilt, &function->instructions[i])) {
        ok = 0;
        goto cleanup;
      }
      if (next_site < site_count && i == sites[next_site].call_index + 1u) {
        if (!ir_live_emit_transfer(context, rebuilt, function, &locals,
                                   &sites[next_site], next_site + 1u)) {
          ok = 0;
          goto cleanup;
        }
        next_site++;
      }
    }
  }
  {
    IRInstruction *instructions = function->instructions;
    size_t count = function->instruction_count;
    size_t capacity = function->instruction_capacity;
    function->instructions = rebuilt->instructions;
    function->instruction_count = rebuilt->instruction_count;
    function->instruction_capacity = rebuilt->instruction_capacity;
    rebuilt->instructions = instructions;
    rebuilt->instruction_count = count;
    rebuilt->instruction_capacity = capacity;
    ir_function_clear_cfg(function);
    ir_function_clear_cfg(rebuilt);
    ir_function_touch(function);
  }
  if (!ir_function_rebuild_cfg(function) ||
      !ir_live_shape_build(function, &shape)) {
    goto restore;
  }
  clones = calloc(site_count, sizeof(IRFunction *));
  if (!clones) {
    ok = 0;
    goto cleanup;
  }
  for (size_t s = 0; s < site_count; s++) {
    clones[s] = ir_live_build_continuation(context, function, &shape, &locals,
                                           &sites[s], s + 1u);
    if (!clones[s]) {
      goto restore;
    }
  }
  for (size_t s = 0; s < site_count; s++) {
    if (!ir_program_add_function(context->program, clones[s])) {
      ok = 0;
      goto cleanup;
    }
    clones[s] = NULL;
  }
  goto cleanup;

restore:
  {
    IRInstruction *instructions = function->instructions;
    size_t count = function->instruction_count;
    size_t capacity = function->instruction_capacity;
    function->instructions = rebuilt->instructions;
    function->instruction_count = rebuilt->instruction_count;
    function->instruction_capacity = rebuilt->instruction_capacity;
    rebuilt->instructions = instructions;
    rebuilt->instruction_count = count;
    rebuilt->instruction_capacity = capacity;
    ir_function_clear_cfg(function);
    ir_function_clear_cfg(rebuilt);
  }
demote:
  ir_live_demote(function);

cleanup:
  if (clones) {
    for (size_t s = 0; s < site_count; s++) {
      if (clones[s]) {
        ir_function_destroy(clones[s]);
      }
    }
    free(clones);
  }
  if (rebuilt) {
    ir_function_destroy(rebuilt);
  }
  ir_live_shape_destroy(&shape);
  ir_live_locals_destroy(&locals);
  free(sites);
  if (ok && !function->cfg_valid && !ir_function_rebuild_cfg(function)) {
    ok = 0;
  }
  return ok;
}

void ir_lower_live_register_symbols(IRProgram *program) {
  if (!program || !g_ir_lowering_live) {
    return;
  }
  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *function = program->functions[i];
    const char *mark;
    char owner[512];
    const IRModuleSymbol *source;
    IRModuleSymbol entry = {0};
    if (!function || !function->name ||
        !(mark = strstr(function->name, IR_LIVE_RESUME_INFIX)) ||
        ir_program_lookup_symbol(program, function->name)) {
      continue;
    }
    size_t length = (size_t)(mark - function->name);
    if (length >= sizeof(owner)) {
      continue;
    }
    memcpy(owner, function->name, length);
    owner[length] = '\0';
    source = ir_program_lookup_symbol(program, owner);
    if (!source) {
      continue;
    }
    entry.name = function->name;
    entry.kind = IR_MODSYM_FUNCTION;
    entry.type = source->type;
    entry.return_type = source->return_type;
    entry.has_body = 1;
    entry.param_types = NULL;
    entry.param_count = 0;
    ir_program_add_symbol(program, &entry);
  }
}
