/* Frontend type-table layout and queries.
 *
 * Offsets, field names, enum members, and pointer/array/slice shape live on
 * Type and are filled here, before const eval, not in the backend. */
#include "symbol_table.h"
#include "string_intern.h"
#include "common.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static size_t align_up(size_t value, size_t alignment) {
  if (alignment <= 1) {
    return value;
  }
  size_t rem = value % alignment;
  return rem ? value + (alignment - rem) : value;
}

int type_alloc_fields(Type *type, size_t field_count) {
  if (!type) {
    return 0;
  }
  type->field_count = field_count;
  if (field_count == 0) {
    type->field_names = NULL;
    type->field_types = NULL;
    type->field_offsets = NULL;
    type->field_bit_offsets = NULL;
    type->field_bit_widths = NULL;
    return 1;
  }
  type->field_names = calloc(field_count, sizeof(char *));
  type->field_types = calloc(field_count, sizeof(Type *));
  type->field_offsets = calloc(field_count, sizeof(size_t));
  type->field_bit_offsets = calloc(field_count, sizeof(uint32_t));
  type->field_bit_widths = calloc(field_count, sizeof(uint32_t));
  if (!type->field_names || !type->field_types || !type->field_offsets ||
      !type->field_bit_offsets || !type->field_bit_widths) {
    return 0;
  }
  return 1;
}

int type_set_field(Type *type, size_t index, const char *name, Type *field_type,
                   uint32_t bit_width) {
  if (!type || index >= type->field_count || !type->field_names) {
    return 0;
  }
  type->field_names[index] = name ? (char *)string_intern(name) : NULL;
  type->field_types[index] = field_type;
  type->field_offsets[index] = 0;
  if (type->field_bit_offsets) {
    type->field_bit_offsets[index] = 0;
  }
  if (type->field_bit_widths) {
    type->field_bit_widths[index] = bit_width;
  }
  return type->field_names[index] != NULL || name == NULL;
}

int type_alloc_enum_members(Type *type, size_t count) {
  if (!type) {
    return 0;
  }
  type->enum_member_count = count;
  if (count == 0) {
    type->enum_member_names = NULL;
    type->enum_member_values = NULL;
    return 1;
  }
  type->enum_member_names = calloc(count, sizeof(char *));
  type->enum_member_values = calloc(count, sizeof(long long));
  return type->enum_member_names && type->enum_member_values;
}

int type_set_enum_member(Type *type, size_t index, const char *name,
                         long long value) {
  if (!type || index >= type->enum_member_count || !type->enum_member_names) {
    return 0;
  }
  type->enum_member_names[index] = name ? (char *)string_intern(name) : NULL;
  type->enum_member_values[index] = value;
  return type->enum_member_names[index] != NULL || name == NULL;
}

static int compute_struct_layout(Type *type) {
  size_t offset = 0;
  size_t max_align = 1;
  size_t unit_end = 0;
  size_t unit_bits = 0;
  size_t unit_used = 0;
  Type *unit_type = NULL;

  for (size_t i = 0; i < type->field_count; i++) {
    Type *ft = type->field_types[i];
    uint32_t width = type->field_bit_widths ? type->field_bit_widths[i] : 0;
    size_t field_align = (ft && ft->alignment) ? ft->alignment : 1;
    size_t field_size = ft ? ft->size : 0;

    if (width == 0) {
      unit_type = NULL;
      unit_used = 0;
      unit_bits = 0;
      offset = align_up(offset > unit_end ? offset : unit_end, field_align);
      if (field_align > max_align) {
        max_align = field_align;
      }
      type->field_offsets[i] = offset;
      if (type->field_bit_offsets) {
        type->field_bit_offsets[i] = 0;
      }
      offset += field_size;
      unit_end = offset;
      continue;
    }

    /* Bitfield: pack into consecutive storage units of the declared type. */
    size_t storage_bits = field_size * 8;
    if (storage_bits == 0 || width > storage_bits) {
      return 0;
    }
    if (field_align > max_align) {
      max_align = field_align;
    }
    int new_unit = !unit_type || unit_type != ft ||
                   unit_used + width > storage_bits;
    if (new_unit) {
      offset = align_up(offset > unit_end ? offset : unit_end, field_align);
      unit_type = ft;
      unit_bits = storage_bits;
      unit_used = 0;
      unit_end = offset + field_size;
    }
    type->field_offsets[i] = offset;
    if (type->field_bit_offsets) {
      type->field_bit_offsets[i] = (uint32_t)unit_used;
    }
    unit_used += width;
    if (unit_used == unit_bits) {
      offset = unit_end;
      unit_type = NULL;
      unit_used = 0;
    }
  }

  if (unit_type) {
    offset = unit_end;
  }
  offset = align_up(offset, max_align);
  type->size = offset;
  type->alignment = max_align ? max_align : 1;
  return 1;
}

int type_compute_layout(Type *type) {
  if (!type) {
    return 0;
  }
  switch (type->kind) {
  case TYPE_POINTER:
    type->size = 8;
    type->alignment = 8;
    return 1;
  case TYPE_SLICE:
    /* { pointer, length }, same shape as string. */
    type->size = 16;
    type->alignment = 8;
    return 1;
  case TYPE_ARRAY:
    if (!type->base_type) {
      return 0;
    }
    if (!type_compute_layout(type->base_type)) {
      return 0;
    }
    if (type->base_type->size > 0 &&
        type->array_size > SIZE_MAX / type->base_type->size) {
      return 0;
    }
    type->size = type->base_type->size * type->array_size;
    type->alignment = type->base_type->alignment
                          ? type->base_type->alignment
                          : 1;
    return 1;
  case TYPE_STRUCT:
  case TYPE_STRING:
    return compute_struct_layout(type);
  case TYPE_TAGGED_ENUM:
    /* Tag + payload union already filled by the enum builder. */
    return type->size > 0;
  default:
    return 1;
  }
}

size_t type_field_count(const Type *type) {
  if (!type) {
    return 0;
  }
  if (type->kind == TYPE_STRUCT || type->kind == TYPE_STRING) {
    return type->field_count;
  }
  return 0;
}

int type_field_at(const Type *type, size_t index, TypeField *out) {
  if (!out || index >= type_field_count(type) || !type->field_names ||
      !type->field_types || !type->field_offsets) {
    return 0;
  }
  out->name = type->field_names[index];
  out->type = type->field_types[index];
  out->byte_offset = type->field_offsets[index];
  out->bit_offset =
      type->field_bit_offsets ? type->field_bit_offsets[index] : 0;
  out->bit_width = type->field_bit_widths ? type->field_bit_widths[index] : 0;
  return 1;
}

int type_field_by_name(const Type *type, const char *name, TypeField *out) {
  int index = type_get_field_index(type, name);
  if (index < 0) {
    return 0;
  }
  return type_field_at(type, (size_t)index, out);
}

size_t type_enum_variant_count(const Type *type) {
  if (!type) {
    return 0;
  }
  if (type->kind == TYPE_ENUM) {
    return type->enum_member_count;
  }
  if (type->kind == TYPE_TAGGED_ENUM) {
    return type->tagged_variant_count;
  }
  return 0;
}

int type_enum_variant_at(const Type *type, size_t index,
                         TypeEnumVariant *out) {
  if (!type || !out) {
    return 0;
  }
  if (type->kind == TYPE_ENUM) {
    if (index >= type->enum_member_count || !type->enum_member_names ||
        !type->enum_member_values) {
      return 0;
    }
    out->name = type->enum_member_names[index];
    out->value = type->enum_member_values[index];
    out->payload = NULL;
    return 1;
  }
  if (type->kind == TYPE_TAGGED_ENUM) {
    if (index >= type->tagged_variant_count || !type->tagged_variant_names) {
      return 0;
    }
    out->name = type->tagged_variant_names[index];
    out->value = type->tagged_variant_tags ? type->tagged_variant_tags[index]
                                           : (long long)index;
    out->payload = type->tagged_variant_payloads
                       ? type->tagged_variant_payloads[index]
                       : NULL;
    return 1;
  }
  return 0;
}

Type *type_pointee(const Type *type) {
  if (!type || type->kind != TYPE_POINTER) {
    return NULL;
  }
  return type->base_type;
}

Type *type_element(const Type *type) {
  if (!type) {
    return NULL;
  }
  if (type->kind == TYPE_ARRAY || type->kind == TYPE_SLICE) {
    return type->base_type;
  }
  return NULL;
}

size_t type_len(const Type *type) {
  if (!type || type->kind != TYPE_ARRAY) {
    return 0;
  }
  return type->array_size;
}

int type_has_static_len(const Type *type) {
  return type && type->kind == TYPE_ARRAY;
}
