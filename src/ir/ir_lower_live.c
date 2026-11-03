#include "ir_lowering_internal.h"
#include "frontend/mtlc_frontend.h"
#include <ctype.h>
#include <stdint.h>

#define IR_LIVE_CHECK_NAME "mettle_live_site_check"
#define IR_LIVE_LEAVE_NAME "mettle_live_site_leave_here"
#define IR_LIVE_NEW_NAME "mettle_live_new"
#define IR_LIVE_MOVED_NAME "mettle_live_frame_moved"
#define IR_LIVE_PUT_NAME "mettle_live_frame_put"
#define IR_LIVE_TAKE_NAME "mettle_live_frame_take"
#define IR_LIVE_ZERO_NAME "mettle_live_frame_zero"
#define IR_LIVE_COPY_NAME "mettle_live_frame_copy"
#define IR_LIVE_END_NAME "mettle_live_frame_end"
#define IR_LIVE_HAND_BACK_NAME "mettle_live_frame_hand_back"
#define IR_LIVE_AGAIN_NAME "mettle_live_frame_again"
#define IR_LIVE_LAST_KEY_NAME "mettle_live_frame_last_key"
#define IR_LIVE_QUIESCE_NAME "mettle_live_quiesce"
#define IR_LIVE_RESUME_SUFFIX "$resume"
#define IR_LIVE_FRAME_SUFFIX "$live_frame"
#define IR_LIVE_BEGIN_MARK "@@live_begin"
#define IR_LIVE_END_MARK "@@live_end"
#define IR_LIVE_KEY_PARAM ".live_resume_key"

typedef struct {
  const char **keys;
  char **values;
  size_t capacity;
  size_t count;
} IRLiveMap;

typedef struct {
  const char *name;
  char *key;
  char *descriptor;
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
  size_t position;
  size_t range_begin;
  int is_quiesce;
  uint64_t key;
  size_t local_count;
  char *resume_label;
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

typedef struct {
  size_t begin;
  size_t end;
  uint64_t hash;
  int is_site;
} IRLiveRange;

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
         !strstr(function->name, IR_LIVE_RESUME_SUFFIX);
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

typedef struct {
  char *text;
  size_t length;
  size_t capacity;
  int failed;
} IRLiveText;

static void ir_live_text_add(IRLiveText *out, const char *text) {
  size_t length = strlen(text);
  if (out->failed) {
    return;
  }
  if (out->length + length + 1u > out->capacity) {
    size_t grown = out->capacity ? out->capacity * 2u : 64u;
    char *next;
    while (grown < out->length + length + 1u) {
      grown *= 2u;
    }
    next = realloc(out->text, grown);
    if (!next) {
      out->failed = 1;
      return;
    }
    out->text = next;
    out->capacity = grown;
  }
  memcpy(out->text + out->length, text, length + 1u);
  out->length += length;
}

static void ir_live_text_number(IRLiveText *out, size_t value) {
  char digits[32];
  snprintf(digits, sizeof(digits), "%zu", value);
  ir_live_text_add(out, digits);
}

static void ir_live_describe(IRLiveText *out, const MtlcType *type,
                             unsigned depth);

static void ir_live_describe_field_value(IRLiveText *out, const Type *owner,
                                         size_t field, unsigned depth) {
  const Type *field_type = owner->field_types[field];
  const TypeMetaPlace *place = type_field_place(owner, field);
  if (place && place->width > 0) {
    int is_signed = field_type->kind == TYPE_INT8 ||
                    field_type->kind == TYPE_INT16 ||
                    field_type->kind == TYPE_INT32 ||
                    field_type->kind == TYPE_INT64;
    ir_live_text_add(out, "P");
    ir_live_text_number(out, field_type->size);
    ir_live_text_add(out, ",");
    ir_live_text_number(out, place->word);
    ir_live_text_add(out, ",");
    ir_live_text_number(out, place->bit);
    ir_live_text_add(out, ",");
    ir_live_text_number(out, place->width);
    ir_live_text_add(out, ",");
    ir_live_text_number(out, place->scale);
    ir_live_text_add(out, ",");
    ir_live_text_number(out, is_signed ? 1u : 0u);
    return;
  }
  ir_live_describe(out, mtlc_type_from_frontend(field_type), depth + 1u);
}

static void ir_live_describe_placed(IRLiveText *out, const Type *type,
                                    unsigned depth) {
  ir_live_text_add(out, "S");
  ir_live_text_number(out, type->size);
  ir_live_text_add(out, "{");
  for (size_t i = 0; i < type->field_count; i++) {
    ir_live_text_add(out, type->field_names[i] ? type->field_names[i] : "_");
    ir_live_text_add(out, ":");
    ir_live_text_number(out, type->field_offsets[i]);
    ir_live_text_add(out, ":");
    ir_live_describe_field_value(out, type, i, depth);
    ir_live_text_add(out, ";");
  }
  ir_live_text_add(out, "}");
}

static void ir_live_describe_columns(IRLiveText *out, const Type *array,
                                     unsigned depth) {
  const Type *element = array->base_type;
  ir_live_text_add(out, "S");
  ir_live_text_number(out, array->size);
  ir_live_text_add(out, "{");
  for (size_t i = 0; i < element->field_count; i++) {
    const TypeMetaPlace *place = type_field_place(element, i);
    ir_live_text_add(out, element->field_names[i] ? element->field_names[i]
                                                  : "_");
    ir_live_text_add(out, ":");
    ir_live_text_number(out, type_columns_region_base(element, place->region,
                                                      array->array_size) +
                                 place->offset);
    ir_live_text_add(out, ":R");
    ir_live_text_number(out, array->array_size);
    ir_live_text_add(out, ",");
    ir_live_text_number(out,
                        element->meta_layout->regions[place->region].stride);
    ir_live_text_add(out, "[");
    ir_live_describe_field_value(out, element, i, depth);
    ir_live_text_add(out, "];");
  }
  ir_live_text_add(out, "}");
}

static void ir_live_describe(IRLiveText *out, const MtlcType *type,
                             unsigned depth) {
  if (!type || depth > 8u) {
    ir_live_text_add(out, "x");
    ir_live_text_number(out, type ? type->size : 0u);
    return;
  }
  switch (type->kind) {
  case MTLC_TYPE_INT8:
  case MTLC_TYPE_INT16:
  case MTLC_TYPE_INT32:
  case MTLC_TYPE_INT64:
    ir_live_text_add(out, "i");
    ir_live_text_number(out, type->size);
    return;
  case MTLC_TYPE_UINT8:
  case MTLC_TYPE_UINT16:
  case MTLC_TYPE_UINT32:
  case MTLC_TYPE_UINT64:
    ir_live_text_add(out, "u");
    ir_live_text_number(out, type->size);
    return;
  case MTLC_TYPE_BOOL:
    ir_live_text_add(out, "b");
    ir_live_text_number(out, type->size);
    return;
  case MTLC_TYPE_ENUM:
    ir_live_text_add(out, "e");
    ir_live_text_number(out, type->size);
    return;
  case MTLC_TYPE_FLOAT32:
  case MTLC_TYPE_FLOAT64:
    ir_live_text_add(out, "f");
    ir_live_text_number(out, type->size);
    return;
  case MTLC_TYPE_POINTER:
  case MTLC_TYPE_FUNCTION_POINTER:
    ir_live_text_add(out, "p8");
    return;
  case MTLC_TYPE_STRING:
    ir_live_text_add(out, "s16");
    return;
  case MTLC_TYPE_ARRAY:
    if (type->frontend_layout_type) {
      ir_live_describe_columns(out, (const Type *)type->frontend_layout_type,
                               depth);
      return;
    }
    ir_live_text_add(out, "A");
    ir_live_text_number(out, type->array_size);
    ir_live_text_add(out, "[");
    ir_live_describe(out, type->base_type, depth + 1u);
    ir_live_text_add(out, "]");
    return;
  case MTLC_TYPE_STRUCT:
    if (type->frontend_layout_type) {
      ir_live_describe_placed(out, (const Type *)type->frontend_layout_type,
                              depth);
      return;
    }
    if (type->field_count && type->field_names && type->field_types &&
        type->field_offsets) {
      ir_live_text_add(out, "S");
      ir_live_text_number(out, type->size);
      ir_live_text_add(out, "{");
      for (size_t i = 0; i < type->field_count; i++) {
        ir_live_text_add(out, type->field_names[i] ? type->field_names[i]
                                                   : "_");
        ir_live_text_add(out, ":");
        ir_live_text_number(out, type->field_offsets[i]);
        ir_live_text_add(out, ":");
        ir_live_describe(out, type->field_types[i], depth + 1u);
        ir_live_text_add(out, ";");
      }
      ir_live_text_add(out, "}");
      return;
    }
    break;
  default:
    break;
  }
  ir_live_text_add(out, "x");
  ir_live_text_number(out, type->size);
}

char *ir_lower_live_descriptor(const MtlcType *type) {
  IRLiveText out = {0};
  ir_live_describe(&out, type, 0u);
  if (out.failed) {
    free(out.text);
    return NULL;
  }
  return out.text;
}

static uint64_t ir_live_type_hash(const MtlcType *type, unsigned depth) {
  uint64_t hash = 1469598103934665603ull;
  if (!type) {
    return hash;
  }
  hash = ir_live_mix(hash, (uint64_t)type->kind);
  hash = ir_live_mix(hash, (uint64_t)type->size);
  hash = ir_live_mix_text(hash, type->name);
  if (type->frontend_layout_type) {
    char *placed = ir_lower_live_descriptor(type);
    hash = ir_live_mix_text(hash, placed);
    free(placed);
  }
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
    free(locals->items[i].descriptor);
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
  local->descriptor = ir_lower_live_descriptor(type);
  if (!local->descriptor) {
    return 0;
  }
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
  IROperand arguments[3];
  IROperand none = ir_operand_none();
  int ok;
  arguments[0] = ir_operand_copy(target);
  arguments[1] = ir_operand_copy(source);
  arguments[2] = ir_operand_int(local->size);
  ok = ir_live_emit_call(context, out, IR_LIVE_COPY_NAME, arguments, 3, &none,
                         location);
  for (size_t i = 0; i < 3; i++) {
    ir_operand_destroy(&arguments[i]);
  }
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

static void ir_live_declare(IRLoweringContext *context, const char *name,
                            const char *shape) {
  IRModuleSymbol entry = {0};
  MtlcType *params[8];
  size_t count = 0;
  if (!context || !context->program || !context->type_checker ||
      ir_program_lookup_symbol(context->program, name)) {
    return;
  }
  MtlcType *word = mtlc_type_from_frontend(context->type_checker->builtin_int64);
  MtlcType *text =
      mtlc_type_from_frontend(context->type_checker->builtin_string);
  for (const char *c = shape; *c && count < 8u; c++) {
    params[count++] = *c == 's' ? text : word;
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
  ir_live_declare(context, IR_LIVE_QUIESCE_NAME, "");
  ir_live_declare(context, IR_LIVE_CHECK_NAME, "isis");
  ir_live_declare(context, IR_LIVE_MOVED_NAME, "isis");
  ir_live_declare(context, IR_LIVE_PUT_NAME, "siiiis");
  ir_live_declare(context, IR_LIVE_TAKE_NAME, "siiis");
  ir_live_declare(context, IR_LIVE_ZERO_NAME, "ii");
  ir_live_declare(context, IR_LIVE_COPY_NAME, "iii");
  ir_live_declare(context, IR_LIVE_END_NAME, "");
  ir_live_declare(context, IR_LIVE_HAND_BACK_NAME, "i");
  ir_live_declare(context, IR_LIVE_AGAIN_NAME, "");
  ir_live_declare(context, IR_LIVE_LAST_KEY_NAME, "");
  ir_live_declare(context, IR_LIVE_LEAVE_NAME, "");
}

static int ir_live_is_site(const IRInstruction *instruction);

static int ir_live_add_leaves(IRFunction *function) {
  int holds = 0;
  for (size_t i = 0; i < function->instruction_count && !holds; i++) {
    holds = ir_live_is_site(&function->instructions[i]);
  }
  if (!holds) {
    return 1;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction leave = {0};
    if (function->instructions[i].op != IR_OP_RETURN) {
      continue;
    }
    leave.op = IR_OP_CALL;
    leave.location = function->instructions[i].location;
    leave.text = IR_LIVE_LEAVE_NAME;
    leave.dest = ir_operand_none();
    if (!ir_function_insert_instruction(function, i, &leave)) {
      return 0;
    }
    i++;
  }
  ir_function_touch(function);
  return 1;
}

int ir_lower_live_marks_statement(const IRFunction *function,
                                  int statement_type) {
  if (!g_ir_lowering_live || !ir_live_function_movable(function)) {
    return 0;
  }
  switch (statement_type) {
  case AST_VAR_DECLARATION:
  case AST_ASSIGNMENT:
  case AST_FUNCTION_CALL:
  case AST_FUNC_PTR_CALL:
  case AST_GPU_LAUNCH:
  case AST_UNARY_EXPRESSION:
  case AST_BINARY_EXPRESSION:
  case AST_MEMBER_ACCESS:
  case AST_INDEX_EXPRESSION:
  case AST_NEW_EXPRESSION:
  case AST_CAST_EXPRESSION:
    return 1;
  default:
    return 0;
  }
}

typedef struct {
  const char *source;
  size_t *starts;
  size_t count;
} IRLiveLines;

static IRLiveLines *g_ir_live_lines = NULL;
static size_t g_ir_live_line_files = 0;

static char **g_ir_live_type_names = NULL;
static char **g_ir_live_type_texts = NULL;
static size_t g_ir_live_type_count = 0;

void ir_lower_live_forget_sources(void) {
  for (size_t i = 0; i < g_ir_live_line_files; i++) {
    free(g_ir_live_lines[i].starts);
  }
  free(g_ir_live_lines);
  g_ir_live_lines = NULL;
  g_ir_live_line_files = 0;
}

void ir_lower_live_forget_heap_types(void) {
  for (size_t i = 0; i < g_ir_live_type_count; i++) {
    free(g_ir_live_type_names[i]);
    free(g_ir_live_type_texts[i]);
  }
  free(g_ir_live_type_names);
  free(g_ir_live_type_texts);
  g_ir_live_type_names = NULL;
  g_ir_live_type_texts = NULL;
  g_ir_live_type_count = 0;
}

size_t ir_lower_live_type_layouts(const char *const **names,
                                  const char *const **layouts) {
  *names = (const char *const *)g_ir_live_type_names;
  *layouts = (const char *const *)g_ir_live_type_texts;
  return g_ir_live_type_count;
}

static int ir_live_remember_type(const char *name, const char *layout) {
  char **names;
  char **texts;
  for (size_t i = 0; i < g_ir_live_type_count; i++) {
    if (strcmp(g_ir_live_type_names[i], name) == 0) {
      return 1;
    }
  }
  names = realloc(g_ir_live_type_names,
                  (g_ir_live_type_count + 1u) * sizeof(char *));
  if (!names) {
    return 0;
  }
  g_ir_live_type_names = names;
  texts = realloc(g_ir_live_type_texts,
                  (g_ir_live_type_count + 1u) * sizeof(char *));
  if (!texts) {
    return 0;
  }
  g_ir_live_type_texts = texts;
  g_ir_live_type_names[g_ir_live_type_count] = mettle_strdup(name);
  g_ir_live_type_texts[g_ir_live_type_count] = mettle_strdup(layout);
  if (!g_ir_live_type_names[g_ir_live_type_count] ||
      !g_ir_live_type_texts[g_ir_live_type_count]) {
    return 0;
  }
  g_ir_live_type_count++;
  return 1;
}

int ir_lower_live_heap_type(const Type *type) {
  return g_ir_lowering_live && type && type->kind == TYPE_STRUCT &&
         type->name && type->size > 0 && !type_is_columnar(type);
}

int ir_lower_live_note_columns(const Type *type) {
  char *layout;
  int ok;
  if (!g_ir_lowering_live || !type_is_columnar(type) || !type->name) {
    return 1;
  }
  layout = ir_lower_live_descriptor(mtlc_type_from_frontend(type));
  if (!layout) {
    return 0;
  }
  layout[0] = 'C';
  ok = ir_live_remember_type(type->name, layout);
  if (!ok) {
    free(layout);
  }
  return ok;
}

int ir_lower_live_new(IRLoweringContext *context, IRFunction *function,
                      const IROperand *pointer, const IROperand *bytes,
                      const Type *type, const IROperand *count,
                      SourceLocation location) {
  IROperand arguments[5];
  char *layout = ir_lower_live_descriptor(mtlc_type_from_frontend(type));
  int ok;
  if (!layout || !ir_live_remember_type(type->name, layout)) {
    free(layout);
    ir_set_error(context, "Out of memory describing a live heap type");
    return 0;
  }
  ir_live_declare(context, IR_LIVE_NEW_NAME, "iiiss");
  arguments[0] = ir_operand_copy(bytes);
  arguments[1] = ir_operand_int((long long)type->size);
  arguments[2] = count ? ir_operand_copy(count) : ir_operand_int(1);
  arguments[3] = ir_operand_string(type->name);
  arguments[4] = ir_operand_string(layout);
  ok = ir_live_emit_call(context, function, IR_LIVE_NEW_NAME, arguments, 5,
                         pointer, location);
  for (size_t i = 0; i < 5; i++) {
    ir_operand_destroy(&arguments[i]);
  }
  free(layout);
  return ok;
}

static const IRLiveLines *ir_live_lines_for(const char *source) {
  IRLiveLines *grown;
  IRLiveLines *entry;
  size_t count = 1;
  size_t at = 1;
  for (size_t i = 0; i < g_ir_live_line_files; i++) {
    if (g_ir_live_lines[i].source == source) {
      return &g_ir_live_lines[i];
    }
  }
  for (const char *c = source; *c; c++) {
    count += *c == '\n';
  }
  grown = realloc(g_ir_live_lines,
                  (g_ir_live_line_files + 1u) * sizeof(IRLiveLines));
  if (!grown) {
    return NULL;
  }
  g_ir_live_lines = grown;
  entry = &g_ir_live_lines[g_ir_live_line_files];
  entry->source = source;
  entry->count = count;
  entry->starts = malloc(count * sizeof(size_t));
  if (!entry->starts) {
    return NULL;
  }
  entry->starts[0] = 0;
  for (size_t i = 0; source[i]; i++) {
    if (source[i] == '\n') {
      entry->starts[at++] = i + 1u;
    }
  }
  g_ir_live_line_files++;
  return entry;
}

static uint64_t ir_live_source_hash(IRLoweringContext *context,
                                    SourceLocation location,
                                    int stop_at_brace) {
  ErrorReporter *reporter = context && context->type_checker
                                ? context->type_checker->error_reporter
                                : NULL;
  const char *source = error_reporter_source_for(reporter, location.filename);
  const IRLiveLines *lines;
  const char *c;
  uint64_t hash = 1469598103934665603ull;
  int depth = 0;
  size_t taken = 0;
  if (!source || location.line == 0) {
    return 0;
  }
  lines = ir_live_lines_for(source);
  if (!lines || location.line > lines->count) {
    return 0;
  }
  c = source + lines->starts[location.line - 1u];
  for (size_t column = 1; column < location.column && *c && *c != '\n';
       column++) {
    c++;
  }
  while (*c && taken < 4096u) {
    char quote;
    if (*c == '/' && c[1] == '/') {
      while (*c && *c != '\n') {
        c++;
      }
      continue;
    }
    if (*c == '\n' && depth <= 0) {
      break;
    }
    if (*c == ' ' || *c == '\t' || *c == '\r' || *c == '\n') {
      c++;
      continue;
    }
    if (depth <= 0 && (*c == ';' || (stop_at_brace && *c == '{'))) {
      break;
    }
    if (*c == '"' || *c == '\'') {
      quote = *c;
      hash = (hash ^ (unsigned char)*c++) * 1099511628211ull;
      while (*c && *c != quote && *c != '\n') {
        if (*c == '\\' && c[1]) {
          hash = (hash ^ (unsigned char)*c++) * 1099511628211ull;
        }
        hash = (hash ^ (unsigned char)*c++) * 1099511628211ull;
        taken++;
      }
      if (*c == quote) {
        hash = (hash ^ (unsigned char)*c++) * 1099511628211ull;
      }
      continue;
    }
    if (*c == '(' || *c == '[' || *c == '{') {
      depth++;
    } else if (*c == ')' || *c == ']' || *c == '}') {
      if (depth <= 0) {
        break;
      }
      depth--;
    }
    hash = (hash ^ (unsigned char)*c++) * 1099511628211ull;
    taken++;
  }
  if (taken == 0) {
    return 0;
  }
  return hash ? hash : 1u;
}

int ir_lower_live_mark(IRLoweringContext *context, IRFunction *function,
                       int begin, SourceLocation location) {
  IRInstruction mark = {0};
  mark.op = IR_OP_NOP;
  mark.location = location;
  mark.text = begin ? IR_LIVE_BEGIN_MARK : IR_LIVE_END_MARK;
  if (begin) {
    mark.lhs = ir_operand_int((long long)ir_live_source_hash(context, location, 0));
  }
  return ir_emit(context, function, &mark);
}

static int ir_live_is_mark(const IRInstruction *instruction, const char *mark) {
  return instruction->op == IR_OP_NOP && instruction->text &&
         strcmp(instruction->text, mark) == 0;
}

static void ir_live_flag_name(const IRFunction *function, char *out,
                              size_t capacity) {
  snprintf(out, capacity, "%s" IR_LIVE_FRAME_SUFFIX, function->name);
}

static int ir_live_emit_address_of(IRLoweringContext *context,
                                   IRFunction *out, const IROperand *dest,
                                   const char *symbol,
                                   SourceLocation location) {
  return ir_live_emit_simple(context, out, IR_OP_ADDRESS_OF,
                             ir_operand_copy(dest), ir_operand_symbol(symbol),
                             ir_operand_none(), NULL, location);
}

static void ir_live_ensure_flag(IRLoweringContext *context,
                                const IRFunction *function) {
  char flag[512];
  IRModuleSymbol entry = {0};
  ir_live_flag_name(function, flag, sizeof(flag));
  if (ir_program_lookup_symbol(context->program, flag)) {
    return;
  }
  entry.name = flag;
  entry.kind = IR_MODSYM_VARIABLE;
  entry.type = mtlc_type_from_frontend(context->type_checker->builtin_int64);
  ir_program_add_symbol(context->program, &entry);
}

int ir_lower_live_site(IRLoweringContext *context, IRFunction *function,
                       SourceLocation location) {
  char flag[512];
  IROperand arguments[4];
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
  ir_live_flag_name(function, flag, sizeof(flag));
  ir_live_ensure_flag(context, function);
  address = ir_live_new_temp(context);
  moved = ir_live_new_temp(context);
  resume = ir_new_label_name(context, "live_resume");
  if (!resume ||
      !ir_live_emit_address_of(context, function, &address, flag, location)) {
    free(resume);
    ir_operand_destroy(&address);
    ir_operand_destroy(&moved);
    return 0;
  }
  arguments[0] = address;
  arguments[1] = ir_operand_string("");
  arguments[2] = ir_operand_int(0);
  arguments[3] = ir_operand_string(function->name);
  ok = ir_live_emit_call(context, function, IR_LIVE_CHECK_NAME, arguments, 4,
                         &moved, location) &&
       ir_live_emit_simple(context, function, IR_OP_BRANCH_ZERO,
                           ir_operand_none(), ir_operand_copy(&moved),
                           ir_operand_none(), resume, location) &&
       ir_emit_label_instruction(context, function, resume, location);
  for (size_t i = 0; i < 4; i++) {
    ir_operand_destroy(&arguments[i]);
  }
  ir_operand_destroy(&moved);
  free(resume);
  return ok;
}

static int ir_live_is_site(const IRInstruction *instruction) {
  return instruction->op == IR_OP_CALL && instruction->text &&
         strcmp(instruction->text, IR_LIVE_CHECK_NAME) == 0;
}

static void ir_live_strip_marks(IRFunction *function) {
  for (size_t i = 0; i < function->instruction_count; i++) {
    IRInstruction *instruction = &function->instructions[i];
    if (ir_live_is_mark(instruction, IR_LIVE_BEGIN_MARK) ||
        ir_live_is_mark(instruction, IR_LIVE_END_MARK)) {
      mettle_free_string(instruction->text);
      instruction->text = NULL;
    }
  }
  ir_function_touch(function);
}

static uint64_t ir_live_operand_hash(uint64_t hash, const IROperand *operand) {
  hash = ir_live_mix(hash, (uint64_t)operand->kind);
  switch (operand->kind) {
  case IR_OPERAND_SYMBOL: {
    char *base = operand->name ? ir_live_base_name(operand->name) : NULL;
    hash = ir_live_mix_text(hash, base);
    free(base);
    break;
  }
  case IR_OPERAND_INT:
    hash = ir_live_mix(hash, (uint64_t)operand->int_value);
    break;
  case IR_OPERAND_FLOAT: {
    uint64_t bits = 0;
    memcpy(&bits, &operand->float_value, sizeof(bits));
    hash = ir_live_mix(hash, bits);
    break;
  }
  case IR_OPERAND_STRING: {
    size_t length = ir_operand_string_length(operand);
    hash = ir_live_mix(hash, length);
    for (size_t i = 0; operand->name && i < length; i++) {
      hash ^= (unsigned char)operand->name[i];
      hash *= 1099511628211ull;
    }
    break;
  }
  default:
    break;
  }
  return hash;
}

static uint64_t ir_live_instruction_hash(uint64_t hash,
                                         const IRInstruction *in) {
  hash = ir_live_mix(hash, (uint64_t)in->op);
  if (in->op != IR_OP_LABEL && in->op != IR_OP_JUMP &&
      in->op != IR_OP_BRANCH_ZERO && in->op != IR_OP_BRANCH_EQ &&
      in->op != IR_OP_NOP) {
    hash = ir_live_mix_text(hash, in->text);
  }
  hash = ir_live_operand_hash(hash, &in->dest);
  hash = ir_live_operand_hash(hash, &in->lhs);
  hash = ir_live_operand_hash(hash, &in->rhs);
  for (size_t a = 0; a < in->argument_count; a++) {
    hash = ir_live_operand_hash(hash, &in->arguments[a]);
  }
  return hash;
}

static uint64_t ir_live_range_hash(const IRFunction *function, size_t from,
                                   size_t to) {
  uint64_t hash = 1469598103934665603ull;
  for (size_t i = from; i < to && i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (in->op == IR_OP_NOP) {
      continue;
    }
    hash = ir_live_instruction_hash(hash, in);
  }
  return hash;
}

static uint64_t ir_live_signature(const IRFunction *function) {
  uint64_t hash = 1469598103934665603ull;
  hash = ir_live_mix(hash, function->parameter_count);
  for (size_t i = 0; i < function->parameter_count; i++) {
    hash = ir_live_mix_text(hash, function->parameter_types
                                      ? function->parameter_types[i]
                                      : NULL);
  }
  return ir_live_mix_text(hash, function->return_type_name);
}

static int ir_live_returns_value(const IRFunction *function) {
  return function->return_type_name &&
         strcmp(function->return_type_name, "void") != 0;
}

static int ir_live_needs_persist(const IRLiveLocal *local) {
  return !local->scalar || local->address_taken;
}

static int ir_live_emit_guard(IRLoweringContext *context, IRFunction *out,
                              const char *count_name, size_t index,
                              const char *skip, SourceLocation location) {
  IROperand test = ir_live_new_temp(context);
  int ok = ir_live_emit_simple(context, out, IR_OP_BINARY,
                               ir_operand_copy(&test),
                               ir_operand_symbol(count_name),
                               ir_operand_int((long long)index), ">",
                               location) &&
           ir_live_emit_simple(context, out, IR_OP_BRANCH_ZERO,
                               ir_operand_none(), ir_operand_copy(&test),
                               ir_operand_none(), skip, location);
  ir_operand_destroy(&test);
  return ok;
}

static int ir_live_emit_declare(IRLoweringContext *context, IRFunction *out,
                                const char *name, const char *type_text,
                                MtlcType *type, SourceLocation location) {
  IRInstruction declare = {0};
  int ok;
  declare.op = IR_OP_DECLARE_LOCAL;
  declare.location = location;
  declare.dest = ir_operand_symbol(name);
  declare.text = (char *)type_text;
  declare.value_type = type;
  ok = ir_emit(context, out, &declare);
  ir_operand_destroy(&declare.dest);
  return ok;
}

static MtlcType *ir_live_word_type(IRLoweringContext *context) {
  return mtlc_type_from_frontend(context->type_checker->builtin_int64);
}

static int ir_live_emit_put(IRLoweringContext *context, IRFunction *out,
                            const IRLiveLocal *local, int bottom,
                            SourceLocation location) {
  IROperand place = ir_live_new_temp(context);
  IROperand arguments[6];
  IROperand none = ir_operand_none();
  int ok = 1;
  int keep_flag = 0;
  if (local->scalar && !local->address_taken) {
    char *save = ir_live_local_name(local->name, ".live");
    IRInstruction assign = {0};
    if (!save) {
      ir_operand_destroy(&place);
      return 0;
    }
    assign.op = IR_OP_ASSIGN;
    assign.location = location;
    assign.dest = ir_operand_symbol(save);
    assign.lhs = ir_operand_symbol(local->name);
    if (local->float_bits) {
      ir_assign_apply_float_bits(&assign, &assign.lhs, local->float_bits);
    }
    ok = ir_live_emit_declare(context, out, save, local->type_text,
                              local->type, location) &&
         ir_emit(context, out, &assign) &&
         ir_live_emit_address_of(context, out, &place, save, location);
    ir_operand_destroy(&assign.dest);
    ir_operand_destroy(&assign.lhs);
    free(save);
  } else {
    ok = ir_live_emit_address_of(context, out, &place, local->name, location);
    keep_flag = !bottom;
  }
  arguments[0] = ir_operand_string(local->key);
  arguments[1] = ir_operand_int(local->layout);
  arguments[2] = ir_operand_int(local->size);
  arguments[3] = place;
  if (bottom) {
    arguments[4] = ir_operand_int(1);
  } else if (keep_flag) {
    char *persist = ir_live_local_name(local->name, ".persist");
    arguments[4] = ir_operand_symbol(persist ? persist : local->name);
    free(persist);
  } else {
    arguments[4] = ir_operand_int(0);
  }
  arguments[5] = ir_operand_string(local->descriptor);
  ok = ok && ir_live_emit_call(context, out, IR_LIVE_PUT_NAME, arguments, 6,
                               &none, location);
  for (size_t i = 0; i < 6; i++) {
    ir_operand_destroy(&arguments[i]);
  }
  return ok;
}

static int ir_live_emit_guarded_puts(IRLoweringContext *context,
                                     IRFunction *out,
                                     const IRLiveLocals *locals, int bottom,
                                     SourceLocation location) {
  for (size_t i = 0; i < locals->count; i++) {
    char *skip = ir_new_label_name(context, "live_put_skip");
    int ok = skip &&
             ir_live_emit_guard(context, out, ".live_count", i, skip,
                                location) &&
             ir_live_emit_put(context, out, &locals->items[i], bottom,
                              location) &&
             ir_emit_label_instruction(context, out, skip, location);
    free(skip);
    if (!ok) {
      return 0;
    }
  }
  return 1;
}

static char *ir_live_resume_name(const char *owner) {
  size_t length = strlen(owner) + sizeof(IR_LIVE_RESUME_SUFFIX);
  char *text = malloc(length);
  if (text) {
    snprintf(text, length, "%s" IR_LIVE_RESUME_SUFFIX, owner);
  }
  return text;
}

static int ir_live_emit_return(IRLoweringContext *context, IRFunction *out,
                               const IRFunction *function,
                               const IROperand *value,
                               SourceLocation location) {
  IRInstruction back = {0};
  back.op = IR_OP_RETURN;
  back.location = location;
  back.lhs = value ? *value : ir_operand_none();
  if (back.lhs.kind != IR_OPERAND_NONE) {
    int bits = ir_named_type_float_bits(context, function->return_type_name);
    if (bits) {
      ir_assign_apply_float_bits(&back, &back.lhs, bits);
    }
  }
  return ir_emit(context, out, &back);
}

static int ir_live_emit_bottom_transfer(IRLoweringContext *context,
                                        IRFunction *out,
                                        const IRFunction *function,
                                        const IRLiveLocals *locals,
                                        const char *transfer_label) {
  SourceLocation location = function->location;
  char *resume = ir_live_resume_name(function->name);
  char *again = ir_new_label_name(context, "live_again");
  char *leave = ir_new_label_name(context, "live_leave");
  IROperand result = ir_operand_none();
  IROperand more = ir_live_new_temp(context);
  IROperand key = ir_live_new_temp(context);
  IROperand none = ir_operand_none();
  IROperand argument = ir_operand_symbol(".live_key");
  Type *return_type = NULL;
  IRInstruction call = {0};
  int ok = resume && again && leave;
  if (ok && ir_live_returns_value(function)) {
    return_type = ir_resolve_named_type(context, function->return_type_name);
    result = ir_live_new_temp(context);
  }
  ok = ok && ir_emit_label_instruction(context, out, transfer_label, location) &&
       ir_live_emit_guarded_puts(context, out, locals, 1, location) &&
       ir_emit_label_instruction(context, out, again, location);
  call.op = IR_OP_CALL;
  call.location = location;
  call.text = resume;
  call.dest = result;
  call.arguments = &argument;
  call.argument_count = 1;
  call.value_type = return_type ? mtlc_type_from_frontend(return_type) : NULL;
  ok = ok && ir_emit(context, out, &call) &&
       ir_live_emit_call(context, out, IR_LIVE_AGAIN_NAME, NULL, 0, &more,
                         location) &&
       ir_live_emit_simple(context, out, IR_OP_BRANCH_ZERO, ir_operand_none(),
                           ir_operand_copy(&more), ir_operand_none(), leave,
                           location) &&
       ir_live_emit_call(context, out, IR_LIVE_LAST_KEY_NAME, NULL, 0, &key,
                         location) &&
       ir_live_emit_simple(context, out, IR_OP_ASSIGN,
                           ir_operand_symbol(".live_key"),
                           ir_operand_copy(&key), ir_operand_none(), NULL,
                           location) &&
       ir_emit_jump_instruction(context, out, again, location) &&
       ir_emit_label_instruction(context, out, leave, location) &&
       ir_live_emit_return(context, out, function,
                           result.kind != IR_OPERAND_NONE ? &result : &none,
                           location);
  ir_operand_destroy(&result);
  ir_operand_destroy(&more);
  ir_operand_destroy(&key);
  ir_operand_destroy(&argument);
  free(resume);
  free(again);
  free(leave);
  return ok;
}

static int ir_live_emit_clone_transfer(IRLoweringContext *context,
                                       IRFunction *out,
                                       const IRFunction *function,
                                       const IRLiveLocals *locals,
                                       const char *transfer_label) {
  SourceLocation location = function->location;
  IROperand argument = ir_operand_symbol(".live_key");
  IROperand none = ir_operand_none();
  IROperand result = ir_live_returns_value(function)
                         ? ir_operand_symbol(".live_ret")
                         : ir_operand_none();
  int ok = ir_emit_label_instruction(context, out, transfer_label, location) &&
           ir_live_emit_guarded_puts(context, out, locals, 0, location) &&
           ir_live_emit_call(context, out, IR_LIVE_HAND_BACK_NAME, &argument,
                             1, &none, location) &&
           ir_live_emit_return(context, out, function,
                               result.kind != IR_OPERAND_NONE ? &result
                                                              : &none,
                               location);
  ir_operand_destroy(&argument);
  ir_operand_destroy(&result);
  return ok;
}

static int ir_live_emit_take(IRLoweringContext *context, IRFunction *out,
                             const IRLiveLocal *local, int want_storage,
                             IROperand *found, SourceLocation location) {
  IROperand arguments[5];
  int ok;
  *found = ir_live_new_temp(context);
  arguments[0] = ir_operand_string(local->key);
  arguments[1] = ir_operand_int(local->layout);
  arguments[2] = ir_operand_int(local->size);
  arguments[3] = ir_operand_int(want_storage);
  arguments[4] = ir_operand_string(local->descriptor);
  ok = ir_live_emit_call(context, out, IR_LIVE_TAKE_NAME, arguments, 5, found,
                         location);
  for (size_t i = 0; i < 5; i++) {
    ir_operand_destroy(&arguments[i]);
  }
  return ok;
}

static int ir_live_emit_restore(IRLoweringContext *context, IRFunction *out,
                                const IRLiveLocal *local, int address_only,
                                SourceLocation location) {
  IROperand found = ir_operand_none();
  IROperand place = ir_operand_none();
  char *fallback = ir_new_label_name(context, "live_default");
  char *done = ir_new_label_name(context, "live_restored");
  char *at = address_only ? ir_live_local_name(local->name, ".at") : NULL;
  char *persist =
      address_only ? ir_live_local_name(local->name, ".persist") : NULL;
  int ok = fallback && done && (!address_only || (at && persist));
  ok = ok &&
       ir_live_emit_take(context, out, local, address_only, &found, location) &&
       ir_live_emit_simple(context, out, IR_OP_BRANCH_ZERO, ir_operand_none(),
                           ir_operand_copy(&found), ir_operand_none(),
                           fallback, location);
  if (ok && address_only) {
    place = ir_live_new_temp(context);
    ok = ir_live_emit_simple(context, out, IR_OP_ASSIGN,
                             ir_operand_symbol(at), ir_operand_copy(&found),
                             ir_operand_none(), NULL, location) &&
         ir_live_emit_simple(context, out, IR_OP_ASSIGN,
                             ir_operand_symbol(persist), ir_operand_int(1),
                             ir_operand_none(), NULL, location) &&
         ir_emit_jump_instruction(context, out, done, location) &&
         ir_emit_label_instruction(context, out, fallback, location) &&
         ir_live_emit_address_of(context, out, &place, local->name,
                                 location) &&
         ir_live_emit_default_memory(context, out, local, &place, location);
  } else if (ok && local->scalar) {
    ok = ir_live_emit_load_into(context, out, local, &found, location) &&
         ir_emit_jump_instruction(context, out, done, location) &&
         ir_emit_label_instruction(context, out, fallback, location) &&
         ir_live_emit_default_scalar(context, out, local, location);
  } else if (ok) {
    place = ir_live_new_temp(context);
    ok = ir_live_emit_address_of(context, out, &place, local->name,
                                 location) &&
         ir_live_emit_copy_bytes(context, out, local, &place, &found,
                                 location) &&
         ir_emit_jump_instruction(context, out, done, location) &&
         ir_emit_label_instruction(context, out, fallback, location) &&
         ir_live_emit_address_of(context, out, &place, local->name,
                                 location) &&
         ir_live_emit_default_memory(context, out, local, &place, location);
  }
  ok = ok && ir_emit_label_instruction(context, out, done, location);
  ir_operand_destroy(&found);
  ir_operand_destroy(&place);
  free(fallback);
  free(done);
  free(at);
  free(persist);
  return ok;
}

static int ir_live_compare_size(const void *a, const void *b) {
  const size_t *x = (const size_t *)a;
  const size_t *y = (const size_t *)b;
  return x[1] < y[1] ? 1 : (x[1] > y[1] ? -1 : 0);
}

static uint64_t ir_live_header_hash(IRLoweringContext *context,
                                    const IRFunction *function,
                                    const IRBasicBlock *header) {
  uint64_t hash = 1469598103934665603ull;
  size_t last_line = 0;
  size_t last_column = 0;
  int found = 0;
  for (size_t i = header->first_instruction;
       i < header->first_instruction + header->instruction_count &&
       i < function->instruction_count;
       i++) {
    const IRInstruction *in = &function->instructions[i];
    uint64_t piece;
    if (in->op == IR_OP_NOP || in->op == IR_OP_LABEL ||
        in->location.line == 0 ||
        (in->location.line == last_line &&
         in->location.column == last_column)) {
      continue;
    }
    last_line = in->location.line;
    last_column = in->location.column;
    piece = ir_live_source_hash(context, in->location, 1);
    if (piece == 0) {
      return ir_live_range_hash(function, header->first_instruction,
                                header->first_instruction +
                                    header->instruction_count);
    }
    hash = ir_live_mix(hash, piece);
    found = 1;
  }
  if (!found) {
    return ir_live_range_hash(function, header->first_instruction,
                              header->first_instruction +
                                  header->instruction_count);
  }
  return hash;
}

static uint64_t ir_live_loop_signature(IRLoweringContext *context,
                                       const IRFunction *function,
                                       const IRLiveShape *shape,
                                       size_t block) {
  size_t (*pairs)[2] = NULL;
  size_t count = 0;
  uint64_t hash = 1469598103934665603ull;
  if (block == SIZE_MAX || shape->rpo_index[block] == SIZE_MAX) {
    return hash;
  }
  pairs = malloc((shape->loop_count ? shape->loop_count : 1u) *
                 sizeof(*pairs));
  if (!pairs) {
    return hash;
  }
  for (size_t l = 0; l < shape->loop_count; l++) {
    if (shape->loop_body[l][block]) {
      pairs[count][0] = shape->loop_header[l];
      pairs[count][1] = shape->loop_size[l];
      count++;
    }
  }
  qsort(pairs, count, sizeof(*pairs), ir_live_compare_size);
  for (size_t i = 0; i < count; i++) {
    const IRBasicBlock *header = &function->blocks[pairs[i][0]];
    hash = ir_live_mix(hash, ir_live_header_hash(context, function, header));
  }
  free(pairs);
  return hash;
}

static int ir_live_site_compare(const void *a, const void *b) {
  const IRLiveSite *x = (const IRLiveSite *)a;
  const IRLiveSite *y = (const IRLiveSite *)b;
  return x->position < y->position ? -1 : (x->position > y->position ? 1 : 0);
}

static void ir_live_hex(char *out, uint64_t value) {
  static const char digits[] = "0123456789abcdef";
  for (int i = 15; i >= 0; i--) {
    out[i] = digits[value & 15u];
    value >>= 4;
  }
}

static char *ir_live_table_text(const IRFunction *function,
                                const IRLiveSite *sites, size_t count) {
  size_t length = 4u + 16u + count * 16u + 1u;
  char *text = malloc(length + 1u);
  if (!text) {
    return NULL;
  }
  memcpy(text, "MTLV", 4u);
  ir_live_hex(text + 4, ir_live_signature(function));
  for (size_t i = 0; i < count; i++) {
    ir_live_hex(text + 20 + i * 16u, sites[i].key);
  }
  text[length - 1u] = '.';
  text[length] = '\0';
  return text;
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

static int ir_live_put_index(IRLiveMap *map, const char *key, size_t value) {
  char *text = malloc(24);
  if (!text) {
    return 0;
  }
  snprintf(text, 24, "%zu", value);
  if (!ir_live_map_put(map, key, text)) {
    free(text);
    return 0;
  }
  return 1;
}

static size_t ir_live_get_index(const IRLiveMap *map, const char *key) {
  const char *text = ir_live_map_get(map, key);
  return text ? (size_t)strtoull(text, NULL, 10) : SIZE_MAX;
}

static IRFunction *ir_live_build_continuation(
    IRLoweringContext *context, IRFunction *function, const IRLiveShape *shape,
    const IRLiveLocals *locals, const IRLiveSite *sites, size_t site_count,
    size_t transfer_start, const char *transfer_label) {
  size_t loops = shape->loop_count;
  size_t *site_loop = calloc(site_count ? site_count : 1u, sizeof(size_t));
  size_t *parent = calloc(loops ? loops : 1u, sizeof(size_t));
  unsigned char *needed = calloc(loops ? loops : 1u, 1u);
  size_t *copy_of = calloc(loops ? loops : 1u, sizeof(size_t));
  size_t *copy_loop = calloc(loops + 1u, sizeof(size_t));
  char **landing = calloc(loops ? loops : 1u, sizeof(char *));
  char **entries = calloc(site_count ? site_count : 1u, sizeof(char *));
  IRLiveMap label_blocks = {0};
  IRLiveMap *labels = NULL;
  IRLiveMap *temps = NULL;
  IRFunction *staging = NULL;
  IRFunction *clone = NULL;
  unsigned char *reachable = NULL;
  unsigned char *address_only = NULL;
  char *name = ir_live_resume_name(function->name);
  char *clone_transfer = ir_new_label_name(context, "live_transfer");
  size_t copy_count = 0;
  SourceLocation location = function->location;
  int ok = 0;

  if (!site_loop || !parent || !needed || !copy_of || !copy_loop || !landing ||
      !entries || !name || !clone_transfer) {
    goto done;
  }
  for (size_t b = 0; b < function->block_count; b++) {
    if (function->blocks[b].label &&
        !ir_live_put_index(&label_blocks, function->blocks[b].label, b)) {
      goto done;
    }
  }
  for (size_t j = 0; j < site_count; j++) {
    size_t block = ir_live_get_index(&label_blocks, sites[j].resume_label);
    size_t best = SIZE_MAX;
    if (block == SIZE_MAX || shape->rpo_index[block] == SIZE_MAX) {
      goto done;
    }
    for (size_t l = 0; l < loops; l++) {
      if (shape->loop_body[l][block] &&
          (best == SIZE_MAX || shape->loop_size[l] < shape->loop_size[best])) {
        best = l;
      }
    }
    site_loop[j] = best;
  }
  for (size_t l = 0; l < loops; l++) {
    size_t best = SIZE_MAX;
    for (size_t m = 0; m < loops; m++) {
      if (m == l || !shape->loop_body[m][shape->loop_header[l]] ||
          shape->loop_size[m] <= shape->loop_size[l]) {
        continue;
      }
      if (best == SIZE_MAX || shape->loop_size[m] < shape->loop_size[best]) {
        best = m;
      }
    }
    parent[l] = best;
  }
  for (size_t j = 0; j < site_count; j++) {
    for (size_t x = site_loop[j]; x != SIZE_MAX; x = parent[x]) {
      needed[x] = 1;
    }
  }
  for (size_t round = 0; round < loops; round++) {
    size_t pick = SIZE_MAX;
    for (size_t l = 0; l < loops; l++) {
      int placed = 0;
      for (size_t c = 0; c < copy_count && !placed; c++) {
        placed = copy_loop[c] == l;
      }
      if (!needed[l] || placed) {
        continue;
      }
      if (pick == SIZE_MAX || shape->loop_size[l] < shape->loop_size[pick]) {
        pick = l;
      }
    }
    if (pick == SIZE_MAX) {
      break;
    }
    copy_of[pick] = copy_count;
    copy_loop[copy_count++] = pick;
  }
  copy_loop[copy_count++] = SIZE_MAX;
  for (size_t l = 0; l < loops; l++) {
    const IRBasicBlock *header;
    if (!needed[l]) {
      continue;
    }
    header = &function->blocks[shape->loop_header[l]];
    if (!header->label || header->instruction_count == 0 ||
        header->instructions[0].op != IR_OP_LABEL) {
      goto done;
    }
    for (size_t b = 0; b < function->block_count; b++) {
      const IRBasicBlock *block = &function->blocks[b];
      const IRInstruction *last;
      if (!shape->loop_body[l][b] || block->instruction_count == 0 ||
          b + 1u != shape->loop_header[l]) {
        continue;
      }
      last = &block->instructions[block->instruction_count - 1u];
      if (last->op != IR_OP_JUMP && last->op != IR_OP_RETURN) {
        goto done;
      }
    }
    landing[l] = ir_new_label_name(context, "live_land");
    if (!landing[l]) {
      goto done;
    }
  }
  labels = calloc(copy_count, sizeof(IRLiveMap));
  temps = calloc(copy_count, sizeof(IRLiveMap));
  staging = ir_function_create(name);
  if (!labels || !temps || !staging) {
    goto done;
  }
  for (size_t j = 0; j < site_count; j++) {
    size_t c = site_loop[j] == SIZE_MAX ? copy_count - 1u
                                        : copy_of[site_loop[j]];
    const char *renamed =
        ir_live_rename(context, &labels[c], sites[j].resume_label, 1);
    if (!renamed) {
      goto done;
    }
    entries[j] = mettle_strdup(renamed);
    if (!entries[j]) {
      goto done;
    }
  }
  {
    char *start = ir_new_label_name(context, "live_start");
    int started = start && ir_emit_label_instruction(context, staging, start,
                                                     location);
    free(start);
    if (!started) {
      goto done;
    }
    for (size_t j = 0; j < site_count; j++) {
      if (!ir_live_emit_simple(context, staging, IR_OP_BRANCH_ZERO,
                               ir_operand_none(), ir_operand_int(0),
                               ir_operand_none(), entries[j], location)) {
        goto done;
      }
    }
    if (!ir_live_emit_simple(context, staging, IR_OP_RETURN, ir_operand_none(),
                             ir_operand_none(), ir_operand_none(), NULL,
                             location)) {
      goto done;
    }
  }
  for (size_t c = 0; c < copy_count; c++) {
    size_t loop = copy_loop[c];
    for (size_t i = 0; i < transfer_start; i++) {
      const IRInstruction *in = &function->instructions[i];
      const char *target = NULL;
      if (in->op == IR_OP_NOP && in->text && strncmp(in->text, "@@", 2) == 0) {
        continue;
      }
      for (size_t l = 0; l < loops; l++) {
        if (needed[l] && parent[l] == loop &&
            i == function->blocks[shape->loop_header[l]].first_instruction &&
            !ir_emit_label_instruction(context, staging, landing[l],
                                       location)) {
          goto done;
        }
      }
      if (ir_live_is_jump(in) && in->text) {
        if (strcmp(in->text, transfer_label) == 0) {
          target = clone_transfer;
        } else {
          size_t to = ir_live_get_index(&label_blocks, in->text);
          size_t block = shape->block_of[i];
          for (size_t y = loop; y != SIZE_MAX && !target; y = parent[y]) {
            if (to == shape->loop_header[y] && shape->loop_body[y][block]) {
              target = landing[y];
            }
          }
        }
      }
      if (!ir_live_copy_instruction(context, staging, in, &labels[c],
                                    &temps[c], target)) {
        goto done;
      }
    }
  }
  if (!ir_live_emit_clone_transfer(context, staging, function, locals,
                                   clone_transfer) ||
      !ir_function_rebuild_cfg(staging)) {
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
  address_only = calloc(locals->count ? locals->count : 1u, 1u);
  clone = ir_function_create(name);
  if (!address_only || !clone) {
    goto done;
  }
  for (size_t i = 0; i < locals->count; i++) {
    address_only[i] =
        (unsigned char)ir_live_address_only(staging, 0, locals->items[i].name);
  }
  {
    const char *parameter_names[1] = {IR_LIVE_KEY_PARAM};
    const char *parameter_types[1] = {"int64"};
    if (!ir_function_set_parameters(clone, parameter_names, parameter_types,
                                    1)) {
      goto done;
    }
  }
  clone->location = location;
  clone->is_noinline = 1;
  if (function->return_type_name) {
    clone->return_type_name = mettle_strdup(function->return_type_name);
    if (!clone->return_type_name) {
      goto done;
    }
  }
  {
    MtlcType *word = ir_live_word_type(context);
    MtlcType *pointer =
        mtlc_type_from_frontend(context->type_checker->builtin_rawptr);
    char *entry = ir_new_label_name(context, "entry");
    char *restore = ir_new_label_name(context, "live_restore");
    IROperand none = ir_operand_none();
    int emitted = entry && restore &&
                  ir_emit_label_instruction(context, clone, entry, location) &&
                  ir_live_emit_declare(context, clone, ".live_key", "int64",
                                       word, location) &&
                  ir_live_emit_declare(context, clone, ".live_count", "int64",
                                       word, location) &&
                  ir_live_emit_declare(context, clone, ".live_n", "int64",
                                       word, location) &&
                  ir_live_emit_simple(context, clone, IR_OP_ASSIGN,
                                      ir_operand_symbol(".live_n"),
                                      ir_operand_int(0), ir_operand_none(),
                                      NULL, location);
    if (emitted && ir_live_returns_value(function)) {
      Type *resolved =
          ir_resolve_named_type(context, function->return_type_name);
      emitted = ir_live_emit_declare(context, clone, ".live_ret",
                                     function->return_type_name,
                                     mtlc_type_from_frontend(resolved),
                                     location);
    }
    for (size_t i = 0; emitted && i < locals->count; i++) {
      const IRLiveLocal *local = &locals->items[i];
      emitted = ir_live_emit_declare(context, clone, local->name,
                                     local->type_text, local->type, location);
      if (emitted && (ir_live_needs_persist(local) || address_only[i])) {
        char *persist = ir_live_local_name(local->name, ".persist");
        emitted = persist &&
                  ir_live_emit_declare(context, clone, persist, "int64", word,
                                       location) &&
                  ir_live_emit_simple(context, clone, IR_OP_ASSIGN,
                                      ir_operand_symbol(persist),
                                      ir_operand_int(0), ir_operand_none(),
                                      NULL, location);
        free(persist);
      }
      if (emitted && address_only[i]) {
        char *at = ir_live_local_name(local->name, ".at");
        IROperand own = ir_live_new_temp(context);
        emitted = at &&
                  ir_live_emit_declare(context, clone, at, "rawptr", pointer,
                                       location) &&
                  ir_live_emit_address_of(context, clone, &own, local->name,
                                          location) &&
                  ir_live_emit_simple(context, clone, IR_OP_ASSIGN,
                                      ir_operand_symbol(at),
                                      ir_operand_copy(&own), ir_operand_none(),
                                      NULL, location);
        ir_operand_destroy(&own);
        free(at);
      }
    }
    for (size_t j = 0; emitted && j < site_count; j++) {
      IROperand same = ir_live_new_temp(context);
      char *next = ir_new_label_name(context, "live_site_next");
      emitted = next &&
                ir_live_emit_simple(context, clone, IR_OP_BINARY,
                                    ir_operand_copy(&same),
                                    ir_operand_symbol(IR_LIVE_KEY_PARAM),
                                    ir_operand_int((long long)sites[j].key),
                                    "==", location) &&
                ir_live_emit_simple(context, clone, IR_OP_BRANCH_ZERO,
                                    ir_operand_none(), ir_operand_copy(&same),
                                    ir_operand_none(), next, location) &&
                ir_live_emit_simple(context, clone, IR_OP_ASSIGN,
                                    ir_operand_symbol(".live_n"),
                                    ir_operand_int(
                                        (long long)sites[j].local_count),
                                    ir_operand_none(), NULL, location) &&
                ir_emit_jump_instruction(context, clone, restore, location) &&
                ir_emit_label_instruction(context, clone, next, location);
      ir_operand_destroy(&same);
      free(next);
    }
    emitted = emitted && ir_emit_label_instruction(context, clone, restore,
                                                   location);
    for (size_t i = 0; emitted && i < locals->count; i++) {
      char *skip = ir_new_label_name(context, "live_restore_skip");
      emitted = skip &&
                ir_live_emit_guard(context, clone, ".live_n", i, skip,
                                   location) &&
                ir_live_emit_restore(context, clone, &locals->items[i],
                                     address_only[i], location) &&
                ir_emit_label_instruction(context, clone, skip, location);
      free(skip);
    }
    emitted = emitted && ir_live_emit_call(context, clone, IR_LIVE_END_NAME,
                                           NULL, 0, &none, location);
    for (size_t j = 0; emitted && j < site_count; j++) {
      IROperand same = ir_live_new_temp(context);
      char *next = ir_new_label_name(context, "live_enter_next");
      emitted = next &&
                ir_live_emit_simple(context, clone, IR_OP_BINARY,
                                    ir_operand_copy(&same),
                                    ir_operand_symbol(IR_LIVE_KEY_PARAM),
                                    ir_operand_int((long long)sites[j].key),
                                    "==", location) &&
                ir_live_emit_simple(context, clone, IR_OP_BRANCH_ZERO,
                                    ir_operand_none(), ir_operand_copy(&same),
                                    ir_operand_none(), next, location) &&
                ir_emit_jump_instruction(context, clone, entries[j],
                                         location) &&
                ir_emit_label_instruction(context, clone, next, location);
      ir_operand_destroy(&same);
      free(next);
    }
    if (emitted) {
      IROperand result = ir_live_returns_value(function)
                             ? ir_operand_symbol(".live_ret")
                             : ir_operand_none();
      emitted = ir_live_emit_return(context, clone, function, &result,
                                    location);
      ir_operand_destroy(&result);
    }
    free(entry);
    free(restore);
    if (!emitted) {
      goto done;
    }
  }
  for (size_t i = 0; i < staging->instruction_count; i++) {
    const IRInstruction *in = &staging->instructions[i];
    const IRLiveLocal *rewrite = NULL;
    if (in->op == IR_OP_ADDRESS_OF && in->lhs.kind == IR_OPERAND_SYMBOL &&
        in->lhs.name) {
      for (size_t l = 0; l < locals->count; l++) {
        if (address_only[l] &&
            strcmp(locals->items[l].name, in->lhs.name) == 0) {
          rewrite = &locals->items[l];
          break;
        }
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
  if (landing) {
    for (size_t l = 0; l < loops; l++) {
      free(landing[l]);
    }
  }
  if (entries) {
    for (size_t j = 0; j < site_count; j++) {
      free(entries[j]);
    }
  }
  if (labels) {
    for (size_t c = 0; c < copy_count; c++) {
      ir_live_map_destroy(&labels[c]);
    }
  }
  if (temps) {
    for (size_t c = 0; c < copy_count; c++) {
      ir_live_map_destroy(&temps[c]);
    }
  }
  ir_live_map_destroy(&label_blocks);
  free(labels);
  free(temps);
  free(site_loop);
  free(parent);
  free(needed);
  free(copy_of);
  free(copy_loop);
  free(landing);
  free(entries);
  free(reachable);
  free(address_only);
  free(name);
  free(clone_transfer);
  if (staging) {
    ir_function_destroy(staging);
  }
  if (!ok && clone) {
    ir_function_destroy(clone);
    clone = NULL;
  }
  return clone;
}

static int ir_live_emit_boundary(IRLoweringContext *context, IRFunction *out,
                                 const IRFunction *function,
                                 const char *table, IRLiveSite *site,
                                 const char *transfer_label) {
  char flag[512];
  SourceLocation location = function->location;
  IROperand address = ir_live_new_temp(context);
  IROperand moved = ir_live_new_temp(context);
  IROperand arguments[4];
  char *resume = ir_new_label_name(context, "live_resume");
  int ok;
  ir_live_flag_name(function, flag, sizeof(flag));
  arguments[0] = ir_operand_copy(&address);
  arguments[1] = ir_operand_string(table);
  arguments[2] = ir_operand_int((long long)site->key);
  arguments[3] = ir_operand_string(function->name);
  ok = resume &&
       ir_live_emit_address_of(context, out, &address, flag, location) &&
       ir_live_emit_call(context, out, IR_LIVE_MOVED_NAME, arguments, 4,
                         &moved, location) &&
       ir_live_emit_simple(context, out, IR_OP_BRANCH_ZERO, ir_operand_none(),
                           ir_operand_copy(&moved), ir_operand_none(), resume,
                           location) &&
       ir_live_emit_simple(context, out, IR_OP_ASSIGN,
                           ir_operand_symbol(".live_key"),
                           ir_operand_int((long long)site->key),
                           ir_operand_none(), NULL, location) &&
       ir_live_emit_simple(context, out, IR_OP_ASSIGN,
                           ir_operand_symbol(".live_count"),
                           ir_operand_int((long long)site->local_count),
                           ir_operand_none(), NULL, location) &&
       ir_emit_jump_instruction(context, out, transfer_label, location) &&
       ir_emit_label_instruction(context, out, resume, location);
  for (size_t i = 0; i < 4; i++) {
    ir_operand_destroy(&arguments[i]);
  }
  ir_operand_destroy(&address);
  ir_operand_destroy(&moved);
  if (ok) {
    site->resume_label = resume;
  } else {
    free(resume);
  }
  return ok;
}

static void ir_live_swap_instructions(IRFunction *a, IRFunction *b) {
  IRInstruction *instructions = a->instructions;
  size_t count = a->instruction_count;
  size_t capacity = a->instruction_capacity;
  a->instructions = b->instructions;
  a->instruction_count = b->instruction_count;
  a->instruction_capacity = b->instruction_capacity;
  b->instructions = instructions;
  b->instruction_count = count;
  b->instruction_capacity = capacity;
  ir_function_clear_cfg(a);
  ir_function_clear_cfg(b);
  ir_function_touch(a);
  ir_function_touch(b);
}

static int ir_live_rebuild(IRLoweringContext *context, IRFunction *function,
                           IRFunction *out, IRLiveSite *sites,
                           size_t site_count, const IRLiveLocals *locals,
                           const char *table, const char *transfer_label,
                           size_t *transfer_start) {
  SourceLocation location = function->location;
  MtlcType *word = ir_live_word_type(context);
  size_t next = 0;
  size_t after_branch = SIZE_MAX;
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (ir_live_is_mark(in, IR_LIVE_BEGIN_MARK)) {
      continue;
    }
    if (ir_live_is_mark(in, IR_LIVE_END_MARK)) {
      if (next < site_count && !sites[next].is_quiesce &&
          sites[next].position == i) {
        if (!ir_live_emit_boundary(context, out, function, table,
                                   &sites[next], transfer_label)) {
          return 0;
        }
        next++;
      }
      continue;
    }
    if (next < site_count && sites[next].is_quiesce &&
        sites[next].position == i) {
      IRInstruction copy = *in;
      IROperand arguments[4];
      int appended;
      if (in->argument_count != 4) {
        return 0;
      }
      arguments[0] = in->arguments[0];
      arguments[1] = ir_operand_string(table);
      arguments[2] = ir_operand_int((long long)sites[next].key);
      arguments[3] = in->arguments[3];
      copy.arguments = arguments;
      appended = ir_function_append_instruction(out, &copy);
      ir_operand_destroy(&arguments[1]);
      ir_operand_destroy(&arguments[2]);
      if (!appended) {
        return 0;
      }
      after_branch = next;
      next++;
      continue;
    }
    if (!ir_function_append_instruction(out, in)) {
      return 0;
    }
    if (i == 0 &&
        (!ir_live_emit_declare(context, out, ".live_key", "int64", word,
                               location) ||
         !ir_live_emit_declare(context, out, ".live_count", "int64", word,
                               location))) {
      return 0;
    }
    if (after_branch != SIZE_MAX && in->op == IR_OP_BRANCH_ZERO) {
      IRLiveSite *site = &sites[after_branch];
      if (i + 1u >= function->instruction_count ||
          function->instructions[i + 1u].op != IR_OP_LABEL ||
          !(site->resume_label =
                mettle_strdup(function->instructions[i + 1u].text)) ||
          !ir_live_emit_simple(context, out, IR_OP_ASSIGN,
                               ir_operand_symbol(".live_key"),
                               ir_operand_int((long long)site->key),
                               ir_operand_none(), NULL, location) ||
          !ir_live_emit_simple(context, out, IR_OP_ASSIGN,
                               ir_operand_symbol(".live_count"),
                               ir_operand_int((long long)site->local_count),
                               ir_operand_none(), NULL, location) ||
          !ir_emit_jump_instruction(context, out, transfer_label, location)) {
        return 0;
      }
      after_branch = SIZE_MAX;
    }
  }
  *transfer_start = out->instruction_count;
  return ir_live_emit_bottom_transfer(context, out, function, locals,
                                      transfer_label);
}

static int ir_live_process(IRLoweringContext *context, IRProgram *program,
                           IRFunction *function, const IRLiveMap *mayq) {
  IRLiveRange *ranges = NULL;
  size_t range_count = 0;
  IRLiveSite *sites = NULL;
  size_t site_count = 0;
  IRLiveLocals locals = {0};
  IRLiveShape shape;
  IRLiveShape rebuilt_shape;
  IRFunction *rebuilt = NULL;
  IRFunction *clone = NULL;
  char *transfer = NULL;
  char *table = NULL;
  size_t stack[256];
  size_t depth = 0;
  size_t transfer_start = 0;
  int usable = 1;
  int swapped = 0;
  int ok = 1;

  memset(&shape, 0, sizeof(shape));
  memset(&rebuilt_shape, 0, sizeof(rebuilt_shape));
  if (!ir_live_function_movable(function)) {
    ir_live_strip_marks(function);
    return 1;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    const IRInstruction *in = &function->instructions[i];
    if (ir_live_is_mark(in, IR_LIVE_BEGIN_MARK)) {
      if (depth < sizeof(stack) / sizeof(stack[0])) {
        stack[depth++] = i;
      } else {
        usable = 0;
      }
    } else if (ir_live_is_mark(in, IR_LIVE_END_MARK)) {
      if (depth == 0) {
        usable = 0;
        continue;
      }
      size_t begin = stack[--depth];
      if (depth == 0) {
        IRLiveRange *grown =
            realloc(ranges, (range_count + 1u) * sizeof(IRLiveRange));
        if (!grown) {
          ok = 0;
          goto finish;
        }
        ranges = grown;
        ranges[range_count].begin = begin;
        ranges[range_count].end = i;
        ranges[range_count].hash =
            function->instructions[begin].lhs.kind == IR_OPERAND_INT &&
                    function->instructions[begin].lhs.int_value != 0
                ? (uint64_t)function->instructions[begin].lhs.int_value
                : ir_live_range_hash(function, begin + 1u, i);
        ranges[range_count].is_site = 0;
        for (size_t k = begin + 1u; k < i; k++) {
          const IRInstruction *call = &function->instructions[k];
          if (call->op == IR_OP_CALL_INDIRECT ||
              (call->op == IR_OP_CALL && call->text &&
               ir_live_map_get(mayq, call->text))) {
            ranges[range_count].is_site = 1;
            break;
          }
        }
        range_count++;
      }
    }
  }
  for (size_t r = 0; r < range_count; r++) {
    if (!ranges[r].is_site) {
      continue;
    }
    IRLiveSite *grown = realloc(sites, (site_count + 1u) * sizeof(IRLiveSite));
    if (!grown) {
      ok = 0;
      goto finish;
    }
    sites = grown;
    memset(&sites[site_count], 0, sizeof(IRLiveSite));
    sites[site_count].position = ranges[r].end;
    sites[site_count].range_begin = ranges[r].begin;
    site_count++;
  }
  for (size_t i = 0; i < function->instruction_count; i++) {
    if (!ir_live_is_site(&function->instructions[i])) {
      continue;
    }
    IRLiveSite *grown = realloc(sites, (site_count + 1u) * sizeof(IRLiveSite));
    if (!grown) {
      ok = 0;
      goto finish;
    }
    sites = grown;
    memset(&sites[site_count], 0, sizeof(IRLiveSite));
    sites[site_count].position = i;
    sites[site_count].range_begin = i;
    sites[site_count].is_quiesce = 1;
    if (i + 2u >= function->instruction_count ||
        function->instructions[i + 1u].op != IR_OP_BRANCH_ZERO ||
        function->instructions[i + 2u].op != IR_OP_LABEL) {
      usable = 0;
    }
    site_count++;
  }
  if (site_count == 0) {
    ir_live_strip_marks(function);
    goto finish;
  }
  qsort(sites, site_count, sizeof(IRLiveSite), ir_live_site_compare);
  if (!usable || !ir_live_collect_locals(context, function, &locals) ||
      !ir_function_rebuild_cfg(function) ||
      !ir_live_shape_build(function, &shape)) {
    goto demote;
  }
  for (size_t j = 0; j < site_count; j++) {
    IRLiveSite *site = &sites[j];
    uint64_t previous = 0;
    uint64_t statement = 0x51u;
    uint64_t combined = 1469598103934665603ull;
    size_t block = site->position < function->instruction_count
                       ? shape.block_of[site->position]
                       : SIZE_MAX;
    for (size_t r = 0; r < range_count; r++) {
      if (ranges[r].end == site->position && !site->is_quiesce) {
        statement = ranges[r].hash;
      }
      if (ranges[r].end < site->range_begin) {
        previous = ranges[r].hash;
      }
    }
    combined = ir_live_mix(combined, (uint64_t)site->is_quiesce);
    combined = ir_live_mix(combined, statement);
    combined = ir_live_mix(combined, previous);
    combined = ir_live_mix(combined,
                           ir_live_loop_signature(context, function, &shape,
                                                  block));
    site->key = combined;
  }
  for (size_t j = site_count; j-- > 0;) {
    size_t occurrence = 0;
    for (size_t k = 0; k < j; k++) {
      occurrence += sites[k].key == sites[j].key;
    }
    sites[j].key = ir_live_mix(sites[j].key, occurrence) & 0x7FFFFFFFFFFFFFFFull;
    if (sites[j].key == 0) {
      sites[j].key = 1;
    }
  }
  for (size_t j = 0; j < site_count; j++) {
    size_t count = 0;
    while (count < locals.count &&
           locals.items[count].declared_at < sites[j].position) {
      count++;
    }
    sites[j].local_count = count;
  }
  ir_live_ensure_flag(context, function);
  table = ir_live_table_text(function, sites, site_count);
  transfer = ir_new_label_name(context, "live_transfer");
  rebuilt = ir_function_create(function->name);
  if (!table || !transfer || !rebuilt) {
    ok = 0;
    goto finish;
  }
  if (!ir_live_rebuild(context, function, rebuilt, sites, site_count, &locals,
                       table, transfer, &transfer_start)) {
    goto demote;
  }
  ir_live_swap_instructions(function, rebuilt);
  swapped = 1;
  if (!ir_function_rebuild_cfg(function) ||
      !ir_live_shape_build(function, &rebuilt_shape)) {
    goto restore;
  }
  clone = ir_live_build_continuation(context, function, &rebuilt_shape,
                                     &locals, sites, site_count,
                                     transfer_start, transfer);
  if (!clone) {
    goto restore;
  }
  function->is_noinline = 1;
  if (!ir_program_add_function(program, clone)) {
    ir_function_destroy(clone);
    ok = 0;
  }
  goto finish;

restore:
  if (swapped) {
    ir_live_swap_instructions(function, rebuilt);
    swapped = 0;
  }
demote:
  ir_live_strip_marks(function);
  ir_live_demote(function);

finish:
  if (rebuilt) {
    ir_function_destroy(rebuilt);
  }
  if (sites) {
    for (size_t j = 0; j < site_count; j++) {
      free(sites[j].resume_label);
    }
  }
  free(sites);
  free(ranges);
  free(table);
  free(transfer);
  ir_live_shape_destroy(&shape);
  ir_live_shape_destroy(&rebuilt_shape);
  ir_live_locals_destroy(&locals);
  if (ok && !function->cfg_valid && !ir_function_rebuild_cfg(function)) {
    ok = 0;
  }
  return ok;
}

int ir_lower_live_program(IRLoweringContext *context, IRProgram *program) {
  IRLiveMap mayq = {0};
  size_t count;
  int changed = 1;
  int ok = 1;
  if (!g_ir_lowering_live || !context || !program) {
    return 1;
  }
  ir_live_declare_helpers(context);
  count = program->function_count;
  for (size_t f = 0; f < count; f++) {
    IRFunction *function = program->functions[f];
    for (size_t i = 0; function && i < function->instruction_count; i++) {
      const IRInstruction *in = &function->instructions[i];
      if (in->op == IR_OP_CALL_INDIRECT || ir_live_is_site(in)) {
        char *mark = mettle_strdup("1");
        if (!mark || !ir_live_map_put(&mayq, function->name, mark)) {
          free(mark);
          ir_live_map_destroy(&mayq);
          return 0;
        }
        break;
      }
    }
  }
  while (changed) {
    changed = 0;
    for (size_t f = 0; f < count; f++) {
      IRFunction *function = program->functions[f];
      if (!function || !function->name ||
          ir_live_map_get(&mayq, function->name)) {
        continue;
      }
      for (size_t i = 0; i < function->instruction_count; i++) {
        const IRInstruction *in = &function->instructions[i];
        if (in->op == IR_OP_CALL && in->text &&
            ir_live_map_get(&mayq, in->text)) {
          char *mark = mettle_strdup("1");
          if (!mark || !ir_live_map_put(&mayq, function->name, mark)) {
            free(mark);
            ir_live_map_destroy(&mayq);
            return 0;
          }
          changed = 1;
          break;
        }
      }
    }
  }
  for (size_t f = 0; f < count && ok; f++) {
    if (program->functions[f]) {
      ok = ir_live_process(context, program, program->functions[f], &mayq);
    }
  }
  for (size_t f = 0; f < program->function_count && ok; f++) {
    if (program->functions[f]) {
      ok = ir_live_add_leaves(program->functions[f]);
    }
  }
  ir_live_map_destroy(&mayq);
  ir_lower_live_forget_sources();
  return ok;
}

void ir_lower_live_register_symbols(IRLoweringContext *context,
                                    IRProgram *program) {
  if (!program || !context || !g_ir_lowering_live) {
    return;
  }
  for (size_t i = 0; i < program->function_count; i++) {
    IRFunction *function = program->functions[i];
    char owner[512];
    const IRModuleSymbol *source;
    IRModuleSymbol entry = {0};
    MtlcType *params[1];
    size_t length;
    if (!function || !function->name ||
        ir_program_lookup_symbol(program, function->name)) {
      continue;
    }
    length = strlen(function->name);
    if (length <= strlen(IR_LIVE_RESUME_SUFFIX) ||
        strcmp(function->name + length - strlen(IR_LIVE_RESUME_SUFFIX),
               IR_LIVE_RESUME_SUFFIX) != 0) {
      continue;
    }
    length -= strlen(IR_LIVE_RESUME_SUFFIX);
    if (length >= sizeof(owner)) {
      continue;
    }
    memcpy(owner, function->name, length);
    owner[length] = '\0';
    source = ir_program_lookup_symbol(program, owner);
    if (!source) {
      continue;
    }
    params[0] = ir_live_word_type(context);
    entry.name = function->name;
    entry.kind = IR_MODSYM_FUNCTION;
    entry.type = source->type;
    entry.return_type = source->return_type;
    entry.has_body = 1;
    entry.param_types = params;
    entry.param_count = 1;
    ir_program_add_symbol(program, &entry);
  }
}
