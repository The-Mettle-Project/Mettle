#include "symbol_table.h"
#include "string_intern.h"
#include "common.h"
#include <stdint.h>
#include <stdio.h>
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

int type_is_columnar(const Type *type) {
  return type && type->meta_layout && type->meta_layout->region_count > 1;
}

int type_is_element_ref(const Type *type) {
  return type && type->is_element_ref;
}

int type_needs_element_ref(const Type *type) {
  return type && (type_is_columnar(type) || type->columnar_hint);
}

const TypeMetaPlace *type_field_place(const Type *type, size_t index) {
  if (!type || !type->meta_layout || index >= type->meta_layout->place_count) {
    return NULL;
  }
  return &type->meta_layout->places[index];
}

int type_field_is_packed(const Type *type, size_t index) {
  const TypeMetaPlace *place = type_field_place(type, index);
  return place && place->width > 0;
}

size_t type_columns_region_base(const Type *type, size_t region, size_t count) {
  size_t base = 0;
  if (!type || !type->meta_layout) {
    return 0;
  }
  for (size_t r = 0; r < region && r < type->meta_layout->region_count; r++) {
    base += type->meta_layout->regions[r].stride * count;
    if (r + 1 < type->meta_layout->region_count) {
      base = align_up(base, type->meta_layout->regions[r + 1].alignment);
    }
  }
  return base;
}

size_t type_columns_bytes(const Type *type, size_t count) {
  if (!type || !type->meta_layout || type->meta_layout->region_count == 0) {
    return 0;
  }
  size_t last = type->meta_layout->region_count - 1;
  return align_up(type_columns_region_base(type, last, count) +
                      type->meta_layout->regions[last].stride * count,
                  type->alignment);
}

static int meta_type_packable(const Type *type) {
  if (!type) {
    return 0;
  }
  switch (type->kind) {
  case TYPE_INT8:
  case TYPE_INT16:
  case TYPE_INT32:
  case TYPE_INT64:
  case TYPE_UINT8:
  case TYPE_UINT16:
  case TYPE_UINT32:
  case TYPE_UINT64:
  case TYPE_BOOL:
  case TYPE_ENUM:
  case TYPE_POINTER:
    return 1;
  default:
    return 0;
  }
}

static int meta_is_power_of_two(size_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}

static size_t meta_place_first_bit(const TypeMetaPlace *place) {
  return place->offset * 8 + place->bit;
}

static size_t meta_place_end_bit(const TypeMetaPlace *place,
                                 const Type *field_type) {
  return meta_place_first_bit(place) +
         (place->width ? place->width : field_type->size * 8);
}

int type_apply_meta_layout(Type *type, TypeMetaLayout *layout, char *why,
                           size_t why_size) {
  TypeMetaPlace *ordered;
  size_t max_align = 1;
  if (!type || type->kind != TYPE_STRUCT || !layout) {
    snprintf(why, why_size, "a layout applies to a struct");
    return 0;
  }
  if (layout->region_count == 0) {
    snprintf(why, why_size, "a layout has at least one region");
    return 0;
  }
  if (layout->place_count != type->field_count) {
    snprintf(why, why_size,
             "'%s' has %zu fields and the layout places %zu of them",
             type->name, type->field_count, layout->place_count);
    return 0;
  }
  for (size_t r = 0; r < layout->region_count; r++) {
    size_t alignment = layout->regions[r].alignment;
    if (!meta_is_power_of_two(alignment)) {
      snprintf(why, why_size,
               "region %zu has alignment %zu, which is not a power of two", r,
               alignment);
      return 0;
    }
    if (layout->regions[r].stride % alignment != 0) {
      snprintf(why, why_size,
               "region %zu has stride %zu, which does not keep its alignment "
               "of %zu across elements",
               r, layout->regions[r].stride, alignment);
      return 0;
    }
    if (alignment > max_align) {
      max_align = alignment;
    }
  }
  ordered = calloc(layout->place_count, sizeof(TypeMetaPlace));
  if (!ordered) {
    snprintf(why, why_size, "out of memory");
    return 0;
  }
  for (size_t f = 0; f < type->field_count; f++) {
    const TypeMetaPlace *found = NULL;
    const Type *field_type = type->field_types[f];
    for (size_t p = 0; p < layout->place_count; p++) {
      if (layout->places[p].field && type->field_names[f] &&
          strcmp(layout->places[p].field, type->field_names[f]) == 0) {
        if (found) {
          snprintf(why, why_size, "the layout places field '%s' twice",
                   type->field_names[f]);
          free(ordered);
          return 0;
        }
        found = &layout->places[p];
      }
    }
    if (!found) {
      snprintf(why, why_size, "the layout does not place field '%s'",
               type->field_names[f]);
      free(ordered);
      return 0;
    }
    if (found->region >= layout->region_count) {
      snprintf(why, why_size, "field '%s' names region %u, and there are %zu",
               type->field_names[f], found->region, layout->region_count);
      free(ordered);
      return 0;
    }
    size_t stride = layout->regions[found->region].stride;
    if (found->width == 0) {
      size_t field_align = field_type->alignment ? field_type->alignment : 1;
      if (found->offset % field_align != 0) {
        snprintf(why, why_size,
                 "field '%s' at offset %zu breaks its alignment of %zu",
                 type->field_names[f], found->offset, field_align);
        free(ordered);
        return 0;
      }
      if (field_align > layout->regions[found->region].alignment) {
        snprintf(why, why_size,
                 "field '%s' needs alignment %zu and region %u has %zu",
                 type->field_names[f], field_align, found->region,
                 layout->regions[found->region].alignment);
        free(ordered);
        return 0;
      }
    } else {
      size_t word = found->word;
      if (!meta_type_packable(field_type)) {
        snprintf(why, why_size,
                 "field '%s' is packed, and only integers, bools, enums and "
                 "pointers can be",
                 type->field_names[f]);
        free(ordered);
        return 0;
      }
      if (word != 1 && word != 2 && word != 4 && word != 8) {
        snprintf(why, why_size,
                 "field '%s' is packed in a word of %zu bytes; a word is 1, 2, "
                 "4 or 8",
                 type->field_names[f], word);
        free(ordered);
        return 0;
      }
      if (found->offset % word != 0 ||
          word > layout->regions[found->region].alignment) {
        snprintf(why, why_size,
                 "the %zu-byte word holding field '%s' is not aligned",
                 word, type->field_names[f]);
        free(ordered);
        return 0;
      }
      if (found->bit + found->width > word * 8) {
        snprintf(why, why_size,
                 "field '%s' takes bits %u to %u of a %zu-bit word",
                 type->field_names[f], found->bit,
                 found->bit + found->width - 1, word * 8);
        free(ordered);
        return 0;
      }
      if (found->width + found->scale > field_type->size * 8) {
        snprintf(why, why_size,
                 "field '%s' has %u stored bits and a shift of %u, wider than "
                 "its %zu-bit type",
                 type->field_names[f], found->width, found->scale,
                 field_type->size * 8);
        free(ordered);
        return 0;
      }
      if (field_type->kind == TYPE_POINTER && field_type->base_type &&
          found->scale > 0 && field_type->base_type->alignment &&
          (1ull << found->scale) >
              (unsigned long long)field_type->base_type->alignment) {
        snprintf(why, why_size,
                 "field '%s' drops %u low bits of a pointer whose target is "
                 "aligned to %zu bytes",
                 type->field_names[f], found->scale,
                 field_type->base_type->alignment);
        free(ordered);
        return 0;
      }
    }
    if (meta_place_end_bit(found, field_type) > stride * 8) {
      snprintf(why, why_size,
               "field '%s' ends past the %zu-byte stride of region %u",
               type->field_names[f], stride, found->region);
      free(ordered);
      return 0;
    }
    ordered[f] = *found;
  }
  for (size_t a = 0; a < type->field_count; a++) {
    for (size_t b = a + 1; b < type->field_count; b++) {
      if (ordered[a].region != ordered[b].region) {
        continue;
      }
      if (meta_place_first_bit(&ordered[a]) <
              meta_place_end_bit(&ordered[b], type->field_types[b]) &&
          meta_place_first_bit(&ordered[b]) <
              meta_place_end_bit(&ordered[a], type->field_types[a])) {
        snprintf(why, why_size, "fields '%s' and '%s' overlap in region %u",
                 type->field_names[a], type->field_names[b], ordered[a].region);
        free(ordered);
        return 0;
      }
    }
  }
  layout->places = ordered;
  type->meta_layout = layout;
  type->alignment = max_align;
  type->size = type_columns_bytes(type, 1);
  for (size_t f = 0; f < type->field_count; f++) {
    type->field_offsets[f] =
        type_columns_region_base(type, ordered[f].region, 1) + ordered[f].offset;
    if (type->field_bit_offsets) {
      type->field_bit_offsets[f] = ordered[f].bit;
    }
    if (type->field_bit_widths) {
      type->field_bit_widths[f] = ordered[f].width;
    }
  }
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
    type->size = 16 * type_view_rank(type);
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
    type->size = type_is_columnar(type->base_type)
                     ? type_columns_bytes(type->base_type, type->array_size)
                     : type->base_type->size * type->array_size;
    type->alignment = type->base_type->alignment
                          ? type->base_type->alignment
                          : 1;
    return 1;
  case TYPE_STRUCT:
  case TYPE_STRING:
    if (type->meta_layout) {
      return 1;
    }
    return compute_struct_layout(type);
  case TYPE_TAGGED_ENUM:
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
