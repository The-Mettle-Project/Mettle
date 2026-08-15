#include "comptime_value.h"

ComptimeValue comptime_none(void) {
  ComptimeValue value;
  value.kind = COMPTIME_NONE;
  value.as.int_value = 0;
  return value;
}

ComptimeValue comptime_int(long long int_value) {
  ComptimeValue value;
  value.kind = COMPTIME_INT;
  value.as.int_value = int_value;
  return value;
}

ComptimeValue comptime_float(double float_value) {
  ComptimeValue value;
  value.kind = COMPTIME_FLOAT;
  value.as.float_value = float_value;
  return value;
}

ComptimeValue comptime_type_ref(uint32_t type_index) {
  ComptimeValue value;
  value.kind = COMPTIME_TYPE_REF;
  value.as.type_ref.type_index = type_index;
  return value;
}

ComptimeValue comptime_field_ref(uint32_t type_index, uint32_t field_index) {
  ComptimeValue value;
  value.kind = COMPTIME_FIELD_REF;
  value.as.field_ref.type_index = type_index;
  value.as.field_ref.field_index = field_index;
  return value;
}

ComptimeValue comptime_string(const char *string_value) {
  ComptimeValue value;
  value.kind = COMPTIME_STRING;
  value.as.string.value = string_value;
  return value;
}

ComptimeValue comptime_sequence(const ComptimeValue *items, uint32_t count) {
  ComptimeValue value;
  value.kind = COMPTIME_SEQUENCE;
  value.as.sequence.items = items;
  value.as.sequence.count = count;
  return value;
}

int comptime_is_none(ComptimeValue value) {
  return value.kind == COMPTIME_NONE;
}

int comptime_is_reflection(ComptimeValue value) {
  return value.kind == COMPTIME_TYPE_REF || value.kind == COMPTIME_FIELD_REF;
}

const char *comptime_kind_name(ComptimeValueKind kind) {
  switch (kind) {
  case COMPTIME_NONE:
    return "none";
  case COMPTIME_INT:
    return "int";
  case COMPTIME_FLOAT:
    return "float";
  case COMPTIME_TYPE_REF:
    return "TypeRef";
  case COMPTIME_FIELD_REF:
    return "FieldRef";
  case COMPTIME_STRING:
    return "string";
  case COMPTIME_SEQUENCE:
    return "sequence";
  }
  return "unknown";
}
